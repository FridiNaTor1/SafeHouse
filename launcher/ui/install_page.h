#pragma once
// A game's install page: installation status, disc image picker (detected
// list for pads, file dialog for mice, drag and drop), install/verify with
// progress. Disc images are identified by their SYSTEM.CNF, so a wrong-game
// image is explained and can be routed to the right game.
#include <algorithm>
#include <deque>
#include <functional>
#include <mutex>
#include <imgui/ImGuiFileDialog.h>
#include "../backend.h"
#include "../install_location.h"
#include "../native_dialog.h"
#include "fixtures.h"
#include "games.h"
#include "iso_scan.h"
#include "job.h"
#include "prompt_bar.h"
#include "shell.h"
#include "widgets.h"

namespace cane::ui {
inline std::string sizeText(uintmax_t bytes) {
    char buf[32];
    if (bytes >= (1ull << 30)) std::snprintf(buf, sizeof(buf), tr("%.2f GB"), double(bytes) / double(1ull << 30));
    else std::snprintf(buf, sizeof(buf), tr("%.0f MB"), double(bytes) / double(1ull << 20));
    return buf;
}

class InstallPage {
public:
    // main.cpp: go back to the game page; install a disc as another game;
    // an installation was started.
    std::function<void()> onBack;
    std::function<void(GameId, std::string)> onRoute;
    std::function<void(GameId)> onStarted;
    // "Change…" beside "Installs to": Settings > Games, where the install folder is chosen.
    std::function<void()> onChangeLocation;

    InstallPage(LauncherBackend& backend, Job& job) : m_backend(backend), m_job(job) {}
    void useFixture(bool installed) {
        m_fixture = true;
        m_fixtureInstalled = installed;
        m_scanner.setResults(fixtures::isos());
        m_scanned = true;
    }
    GameId game() const { return m_game; }
    bool installed() {
        if (m_fixture) return m_fixtureInstalled;
        const double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
        const bool busy = m_job.busy.load();
        if ((m_wasBusy && !busy) || (!busy && now >= m_nextCheck)) {
            m_installed = m_backend.assetsInstalled(m_game);
            m_repair = m_backend.needsRepair(m_game);
            m_outdated = m_backend.needsReextract(m_game);
            m_regions = m_backend.installations(m_game);
            m_region = m_backend.selectedRegion(m_game);
            m_nextCheck = now + 1.0;
        }
        m_wasBusy = busy;
        return m_installed;
    }
    void invalidate() { m_nextCheck = 0; }
    // The page opens for a game: start looking for disc images, and pick the
    // remembered or best matching one.
    // addRegion: "Install another region" (Settings): the page offers a disc of a region not installed yet.
    void open(GameId game, std::string iso = {}, bool addRegion = false) {
        if (game != m_game) m_pathOpen = false, m_note.clear();
        m_game = game;
        m_addRegion = addRegion;
        m_checkPath.clear();
        if (!m_fixture) {
            m_regions = m_backend.installations(game);
            m_region = m_backend.selectedRegion(game);
        }
        m_dismissed = m_job.message;
        invalidate();
        if (!m_scanned) {
            m_scanner.scan();
            m_scanned = true;
        }
        if (!iso.empty()) m_iso = std::move(iso);
        else if (m_fixture) m_iso = m_fixtureInstalled ? std::string{} : bestCandidate();
        else {
            m_iso = addRegion ? std::string{} : m_backend.rememberedIso(game);
            if (m_iso.empty()) m_iso = bestCandidate();
        }
        m_choseFirst = true;
    }
    bool addingRegion() const { return m_addRegion; }
    // Starts the install of the chosen image at once (the game page's "Re-extract").
    void installNow() {
        if (!m_job.busy && !m_iso.empty()) startInstall(m_iso);
    }
    // Screenshots: the regions a fixture shows as installed.
    void fixtureRegions(std::vector<Installation> regions, std::string selected) {
        m_regions = std::move(regions);
        m_region = std::move(selected);
    }
    // Shows a job-finished banner on the next open (main.cpp).
    void dropped(const char* file) {
        if (!m_job.busy && file) m_iso = file;
    }
    std::vector<launcher::Hint> hints() const {
        using input::Button;
        if (m_job.busy) return { { backButton(), GLFW_KEY_ESCAPE, N_("Back") }, { confirmButton(), GLFW_KEY_ENTER, N_("Select") } };
        std::vector<launcher::Hint> h{ { Button::West, 0, N_("Enter path") } };
        if (!(m_fixture ? m_fixtureInstalled : m_installed) || m_addRegion) h.push_back({ Button::Start, 0, N_("Install") });
        h.push_back({ backButton(), GLFW_KEY_ESCAPE, N_("Back") });
        h.push_back({ confirmButton(), GLFW_KEY_ENTER, N_("Select") });
        return h;
    }
    // X is a pad button, so it always opens the pad-friendly path entry.
    void actionX() { openPathEntry(false); }
    // Start installs only when the game is missing: a reinstall over a
    // working install takes minutes, so it needs the Reinstall button.
    void primaryAction(bool sessionActive) {
        if (!m_job.busy && !sessionActive && (!installed() || m_addRegion)) install();
    }
    // B/Escape: close the path entry first.
    bool back() {
        if (!m_pathOpen) return false;
        m_pathOpen = false;
        m_installAfterSelection = false;
        requestFocus();
        return true;
    }
    // Fixture: the page as it looks while installing.
    void showProgressFixture() { m_fixtureProgress = true; }
    void draw(bool sessionActive) {
        const bool busy = m_job.busy.load() && (m_fixtureProgress || m_jobGame == m_game);
        const bool otherJob = m_job.busy.load() && !busy;
        sessionActive = sessionActive || otherJob;
        const bool ready = installed();
        std::string message, stage, item;
        float progress = 0;
        std::deque<std::string> lines;
        {
            std::lock_guard lock(m_job.mutex);
            message = m_job.message;
            stage = m_job.stage;
            item = m_job.item;
            progress = m_job.progress;
            if (m_showLog) lines = m_job.lines;
        }
        const GameInfo& g = cane::game(m_game);
        const std::string title = m_addRegion ? trf("Install another region of %s", g.shortName) : trf("Install %s", g.shortName);
        const std::string sub = m_addRegion
                                    ? std::string(tr("Each region installs side by side; a dropdown on the game page picks the one to play. Saves are shared."))
                                    : trf("SafeHouse installs %s from your own disc image (%s).", g.title, supportedRegionList(m_game).c_str());
        if (titleBar(title.c_str(), true, sub.c_str()) && onBack) onBack();
        if (m_choseFirst) {
            // Opening the page puts the focus on the chosen image or Install.
            m_choseFirst = false;
            requestFocus();
        }
        if (!busy && !otherJob && message != "Ready" && message != m_dismissed && m_jobGame == m_game) {
            const bool ok = message == "Completed";
            if (banner("job-result", ok ? (m_lastLabel.empty() ? std::string(tr("Done.")) : trf("%s finished.", tr(m_lastLabel).c_str())) : tr(message),
                       ok ? BannerTone::Success : message == "Cancelled" ? BannerTone::Info : BannerTone::Error))
                m_dismissed = message;
        }
        if (busy) m_lastLabel = m_job.label;
        if (!m_note.empty() && banner("install-note", m_note, BannerTone::Warning)) m_note.clear();
        const float W = ImGui::GetContentRegionAvail().x;
        statusCard(W, ready, busy, sessionActive);
        gap(4);
        if (busy) progressCard(W, stage, message, item, progress, lines);
        else isoCard(W, ImGui::GetContentRegionAvail().y - px(4), ready, sessionActive);
    }
    void drawDialogs() {
        // The operating system's dialog (native_dialog.h) runs on its own thread; its answer arrives here.
        if (auto r = m_native.poll()) {
            if (r->status == filedialog::Status::Chosen) {
                rememberFolder(path(r->path).parent_path());
                if (!IsoScanner::isIso(path(r->path)))
                    m_note = tr("That file is not a disc image (.iso or .chd).");
                else {
                    m_iso = r->path;
#if defined(CANE_ANDROID) || defined(CANE_IOS)
                    if (!m_fixture) m_scanner.scan();  // (the copy in files/imports joins the list, iso_scan.h)
#endif
                    if (m_installAfterSelection) startInstall(m_iso);
                }
                m_installAfterSelection = false;
            } else if (r->status == filedialog::Status::Unavailable)
                openLauncherPicker();  // no portal / zenity / kdialog here
            else {
                if (r->status == filedialog::Status::Failed) m_note = r->error;
                m_installAfterSelection = false;
            }
        }
        if (!ImGuiFileDialog::Instance()->Display("iso", ImGuiWindowFlags_NoCollapse, { px(650), px(400) }, { px(1100), px(760) }))
            return;
        if (ImGuiFileDialog::Instance()->IsOk()) {
            m_iso = ImGuiFileDialog::Instance()->GetFilePathName();
            rememberFolder(path(m_iso).parent_path());
            if (m_installAfterSelection) startInstall(m_iso);
        }
        m_installAfterSelection = false;
        ImGuiFileDialog::Instance()->Close();
    }
    std::string error;

private:
    void openFolderSafe(const fs::path& folder) {
        try {
            openFolder(folder);
        } catch (const std::exception& e) {
            std::lock_guard lock(m_job.mutex);
            m_job.message = e.what();
        }
    }
    // ImGuiFileDialog cannot be driven with a pad, so pad users type a folder
    // or .iso path instead (with the on-screen keyboard).
    static bool padUser() { return input::tracker().lastWasGamepad(); }
    void choose(bool installAfter) {
#if defined(CANE_ANDROID) || defined(CANE_IOS)
        // Android / iOS: the system document picker works with a pad too (native_dialog.h); typed paths cannot reach it
        if (!m_fixture) browse(installAfter);
        return;
#endif
        if (padUser()) openPathEntry(installAfter);
        else if (!m_fixture) browse(installAfter);
    }
    void openPathEntry(bool installAfter) {
        if (m_job.busy) return;
        m_pathOpen = true;
        m_pathFocus = true;
        m_installAfterSelection = installAfter;
        m_pathNote.clear();
    }
    // A folder is searched (two levels deep, like the usual places); an .iso
    // file is chosen, and installed when the entry was opened by Install.
    void submitPath() {
        std::string text = m_pathBuf;
        const auto first = text.find_first_not_of(" \t\"'");
        const auto last = text.find_last_not_of(" \t\"'");
        text = first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
        if (text.empty()) return;
        fs::path chosen = path(text);
#ifndef _WIN32
        if (text == "~" || text.rfind("~/", 0) == 0)
            if (const char* home = std::getenv("HOME"); home && *home) chosen = path(home) / path(text.size() > 2 ? text.substr(2) : std::string{});
#endif
        std::error_code ec;
        if (fs::is_regular_file(chosen, ec)) {
            if (!IsoScanner::isIso(chosen)) {
                m_pathNote = tr("That file is not a disc image (.iso or .chd).");
                return;
            }
            m_iso = utf8(chosen);
            m_pathOpen = false;
            if (m_installAfterSelection && !m_fixture) startInstall(m_iso);
        } else if (fs::is_directory(chosen, ec)) {
            if (!m_fixture) m_scanner.scan({ chosen });
            m_pathOpen = false;
        } else {
            m_pathNote = tr("Nothing found at that path.");
            return;
        }
        m_installAfterSelection = false;
        m_pathBuf[0] = 0;
        requestFocus();
    }
    void pathEntry(float w) {
        gap(10);
        field(tr("Folder or disc image"), tr("Folders are searched two levels deep"));
        const float searchW = px(120), cancelW = px(100);
        const bool entered = inputField("##iso-path", m_pathBuf, sizeof(m_pathBuf), launcher::Charset::Free,
#ifdef _WIN32
                                        tr("e.g. D:\\Games or D:\\Games\\Sly.iso"),
#elif defined(__APPLE__)
                                        tr("e.g. ~/Downloads/Sly.iso or /Volumes/Games"),
#else
                                        tr("e.g. /run/media/deck/SD/Emulation/roms/ps2"),
#endif
                                        w - searchW - cancelW - px(16), ImGuiInputTextFlags_EnterReturnsTrue);
        // Opening the entry focuses the field. A pad user confirms it to open
        // the on-screen keyboard (padInputText opens it on a press, not on
        // activation); a keyboard user can type at once, so the field is also
        // activated. SetKeyboardFocusHere cannot reach items in nav-flattened
        // children, so both go by ID.
        if (m_pathFocus) {
            m_pathFocus = false;
            const ImGuiID id = ImGui::GetItemID();
            setFocus(id, ImGui::GetCurrentWindow(), true);
            if (!padUser()) {
                ImGui::ActivateItemByID(id);
                GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
            }
        }
        if (entered) submitPath();
        ImGui::SameLine(0, px(8));
        ImGui::BeginDisabled(!m_pathBuf[0]);
        if (button(tr("Search"), Tone::Primary, { searchW, 0 }, Icon::Search)) submitPath();
        ImGui::EndDisabled();
        ImGui::SameLine(0, px(8));
        if (button(tr("Cancel"), Tone::Quiet, { cancelW, 0 })) back();
        if (!m_pathNote.empty()) {
            tighten(2);
            text(m_pathNote.c_str(), fonts().caption, color::warning);
        }
        gap(6);
    }
    // Browse: the operating system's file dialog where there is one (and the launcher is not in Game Mode / Big
    // Picture), else the in-launcher one. It opens in the folder last used, else Downloads / home.
    std::string browseTitle() const { return trf("Choose your %s disc image", cane::game(m_game).shortName); }
    fs::path browseFolder() const {
        return filedialog::startFolder({ m_iso, m_backend.config.value("browse_dir", std::string{}) });
    }
    void rememberFolder(const fs::path& folder) {
        std::error_code ec;
        if (folder.empty() || !fs::is_directory(folder, ec)) return;
        m_backend.config["browse_dir"] = utf8(folder);
        try {
            m_backend.saveConfig();
        } catch (...) {
        }
    }
    void browse(bool installAfter) {
        if (m_job.busy || m_native.running()) return;
        m_installAfterSelection = installAfter;
        if (!m_native.start(filedialog::Kind::OpenIso, browseTitle(), browseFolder())) openLauncherPicker();
    }
    void openLauncherPicker() {
        ImGuiFileDialog::Instance()->OpenDialog("iso", browseTitle().c_str(), "Disc images{.iso,.ISO,.chd,.CHD}", utf8(browseFolder()), "", 1, nullptr,
                                                ImGuiFileDialogFlags_Modal);
    }
    void install() {
        if (m_fixture) {
            if (m_iso.empty()) choose(true);
            return;
        }
        if (m_iso.empty() || !fs::is_regular_file(path(m_iso))) choose(true);
        else startInstall(m_iso);
    }
    void startInstall(const std::string& selected) {
        // Refuse a disc image of another game, an unsupported region or a damaged image before anything is copied.
        const auto& check = discCheck(selected);
        if (!check.ok()) {
            m_note = check.message;
            return;
        }
        m_note.clear();
        m_dismissed.clear();
        m_jobGame = m_game;
        const GameId game = m_game;
        const bool replaces = std::any_of(m_regions.begin(), m_regions.end(), [&](const auto& r) { return r.region == check.region->key; });
        m_job.start(replaces ? "Reinstall" : "Installation", [this, selected, game](const auto& log) {
            m_backend.install(selected, game, m_job.cancel, log);
        });
        if (onStarted) onStarted(game);
        invalidate();
    }
    void statusCard(float width, bool ready, bool busy, bool sessionActive) {
        const bool needsRepair = !ready && !m_fixture && m_repair && !m_addRegion;
        const bool outdated = needsRepair && m_outdated;
        if (m_addRegion) ready = false;
        beginCard("install-status", { width, 0 }, 20);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        auto* dl = ImGui::GetWindowDrawList();
        const float badge = px(52);
        const uint32_t tone = busy ? color::cta : ready ? color::success : needsRepair ? color::warning : color::cta;
        const ImVec2 c{ p.x + badge * 0.5f, p.y + badge * 0.5f };
        dl->AddCircleFilled(c, badge * 0.5f, ink(tone, 0.16f), 40);
        if (busy) spinner(dl, c, px(14));
        else drawIcon(dl, ready ? Icon::Check : needsRepair ? Icon::Warning : Icon::Disc, c, px(26), ink(tone));
        ImGui::SetCursorScreenPos({ p.x + badge + px(18), p.y + px(2) });
        ImGui::BeginGroup();
        const std::string& label = m_job.label;
        std::string installedList;
        for (const auto& r : m_regions) installedList += (installedList.empty() ? "" : ", ") + tr(r.name());
        const std::string regionSuffix = m_regions.size() > 1 ? " (" + tr(selectedRegionName()) + ")" : std::string{};
        heading(busy ? (label == "Verification" ? tr("Checking your files…") : tr("Installing…"))
                : m_addRegion ? tr("Install another region")
                : ready ? (tr("Ready to play") + regionSuffix).c_str() : outdated ? (tr("Update needed") + regionSuffix).c_str()
                : needsRepair ? tr("Needs repair") : tr("Not installed yet"));
        tighten(8);
        std::string sub;
        if (busy)
            sub = tr("This takes a few minutes. Keep the launcher open until it finishes.");
        else if (ready) {
            const auto from = m_backend.rememberedIso(m_game);
            sub = from.empty() || m_fixture ? std::string(tr("Game files are installed and verified.")) : trf("Installed from %s", utf8(path(from).filename()).c_str());
        } else if (m_addRegion)
            sub = (installedList.empty() ? std::string(tr("No region is installed yet.")) : trf("Installed: %s.", installedList.c_str())) + " " +
                  tr("Choose a disc image of another region below.");
        else if (outdated)
            sub = tr("A SafeHouse update changed the format of the game files. Extract your disc again (the image you installed from is "
                  "selected below). Your saves are kept.");
        else if (needsRepair)
            sub = tr("This installation is damaged or incomplete. Choose your disc image and reinstall.");
        else
            sub = trf("Choose your disc image below, then install. It needs %s of free space.", spaceRange(m_game).c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - px(48) - badge - px(18) - px(190));
        text(sub.c_str(), fonts().caption, color::text2);
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        if (ready && !busy) {
            ImGui::SameLine(0, 0);
            ImGui::SetCursorScreenPos({ p.x + width - px(40) - px(170), p.y + px(8) });
            ImGui::BeginDisabled(busy || sessionActive);
            if (button(tr("Verify files"), Tone::Secondary, { px(170), 0 }, Icon::Check) && !m_fixture)
                m_jobGame = m_game, m_job.start("Verification", [this, data = m_backend.dataDir(m_game)](const auto& log) {
                    verifyInstallation(
                        data,
                        [&](size_t n, size_t total, const std::string& name) {
                            log(json({ { "stage", "verify" }, { "completed", n }, { "total", total }, { "item", name } }).dump());
                        },
                        [&] { return m_job.cancel.load(); });
                });
            ImGui::EndDisabled();
        }
        ImGui::SetCursorScreenPos({ p.x, p.y + badge });
        ImGui::Dummy({ 1, 0 });
        endCard();
    }
    void progressCard(float width, const std::string& stage, const std::string& message, const std::string& item, float progress,
                      const std::deque<std::string>& lines) {
        beginCard("install-progress", { width, m_showLog ? ImGui::GetContentRegionAvail().y : 0.0f }, 24);
        overline(m_job.label.c_str());
        heading(stage.empty() ? message.c_str() : stageLabel(stage).c_str());
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, { p.x + w, p.y + px(8) }, ink(color::border), px(4));
        dl->AddRectFilled(p, { p.x + w * std::clamp(progress, 0.0f, 1.0f), p.y + px(8) }, ink(color::cta), px(4));
        ImGui::Dummy({ w, px(8) });
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%.0f%%", progress * 100);
        text(pct, fonts().tab, color::text);
        if (!item.empty()) {
            ImGui::SameLine(0, px(12));
            text(item.c_str(), fonts().caption, color::muted);
        }
        gap(4);
        if (button(m_showLog ? tr("Hide details") : tr("Show details"), Tone::Quiet, { 0, 0 }, m_showLog ? Icon::ChevronDown : Icon::ChevronRight))
            m_showLog = !m_showLog;
        ImGui::SameLine(0, px(8));
        if (button(tr("Cancel"), Tone::Danger, { px(120), 0 })) m_job.cancel = true;
        takeFocus(!input::tracker().lastWasMouse());
        if (m_showLog) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(color::bg));
            ImGui::BeginChild("install-log", { 0, ImGui::GetContentRegionAvail().y }, ImGuiChildFlags_Border);
            {
                FontScope f(fonts().caption);
                ColorScope c(color::text2);
                for (const auto& line : lines) ImGui::TextUnformatted(line.c_str());
                ImGui::SetScrollHereY(1);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        endCard();
    }
    void isoCard(float width, float height, bool ready, bool sessionActive) {
        beginCard("iso-card", { width, height }, 20);
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 row = ImGui::GetCursorScreenPos();
        // Room for the heading next to the Search again / Browse buttons.
#if defined(CANE_ANDROID) || defined(CANE_IOS)
        const char* browseLabel = tr("Browse…###browse");
#else
        const char* browseLabel = padUser() ? tr("Enter path…###browse") : tr("Browse…###browse");
#endif
        const float browseW = std::max(px(130), fonts().body->CalcTextSizeA(fonts().body->FontSize, FLT_MAX, 0.0f, browseLabel, ImGui::FindRenderedTextEnd(browseLabel)).x + px(68));
        const float rescanW = controlHeight();
        ImGui::BeginGroup();
        heading(tr("Disc image"));
        tighten(8);
        ImGui::PushClipRect(ImGui::GetCursorScreenPos(), { row.x + w - browseW - rescanW - px(20), row.y + px(80) }, true);
#if defined(CANE_ANDROID) || defined(CANE_IOS)
        const char* where = tr("Disc images you picked with Browse…");
#elif defined(_WIN32)
        const char* where = tr("Found in your Downloads, Documents and Desktop folders");
#else
        const char* where = tr("Found in your Emulation, Games, Downloads, Desktop and Documents folders and on removable drives");
#endif
        {
            // (two lines when a translation is longer than the room beside the buttons)
            const float wrapRight = row.x + w - browseW - rescanW - px(20);
            FontScope f(fonts().caption);
            ColorScope c(color::muted);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (wrapRight - ImGui::GetCursorScreenPos().x));
            ImGui::TextUnformatted(m_scanner.scanning() ? tr("Searching your folders…") : where);
            ImGui::PopTextWrapPos();
        }
        ImGui::PopClipRect();
        ImGui::EndGroup();
        // Pad users get the path entry instead of the mouse-only file dialog.
        sameRow(row, w - browseW - rescanW - px(8));
        if (iconButton(tr("Search again"), Icon::Refresh, rescanW) && !m_fixture) m_scanner.scan();
        ImGui::SameLine(0, px(8));
        ImGui::BeginDisabled(m_native.running());  // the system's file dialog is open in its own window
        if (button(browseLabel, Tone::Secondary, { browseW, 0 }, padUser() ? Icon::Search : Icon::Folder)) choose(false);
        ImGui::EndDisabled();
        if (m_pathOpen) pathEntry(w);
        if (m_iso.empty() && !m_scanner.scanning()) m_iso = bestCandidate();
        auto found = m_scanner.results();
        // Keep the chosen file in the list even when the scan did not find it.
        if (!m_iso.empty() && std::none_of(found.begin(), found.end(), [&](const auto& c) { return c.path == path(m_iso); })) {
            IsoCandidate chosen;
            chosen.path = path(m_iso);
            std::error_code ec;
            chosen.size = fs::file_size(chosen.path, ec);
            chosen.identity = identifyDiscImage(chosen.path);
            found.insert(found.begin(), chosen);
        }
        // Images of this game first, the rest after them.
        std::stable_sort(found.begin(), found.end(), [&](const auto& a, const auto& b) { return matches(a) && !matches(b); });
        const IsoCandidate* selected = nullptr;
        for (const auto& c : found)
            if (!m_iso.empty() && c.path == path(m_iso)) selected = &c;
        const DiscCheck* check = selected && !m_fixture ? &discCheck(m_iso) : nullptr;
        if (selected && m_fixture) {
            // (fixtures: the identity alone, no file to read)
            static DiscCheck fake;
            fake = {};
            fake.identity = selected->identity;
            fake.game = selected->identity.game;
            if (fake.game) fake.region = regionByBootFile(*fake.game, selected->identity.bootFile);
            const auto why = discMismatch(selected->identity, m_game);
            fake.status = why.empty() ? DiscCheck::Status::Ok : DiscCheck::Status::WrongGame;
            fake.message = why;
            if (fake.region && fake.ok()) fake.release = fake.region->name;
            check = &fake;
        }
        const std::string mismatch = check && !check->ok() ? check->message : std::string{};
        const bool canRoute = check && check->status == DiscCheck::Status::WrongGame && check->game && installable(*check->game) &&
                              check->region && check->region->supported;
        const std::string info = check && check->ok() ? discInfo(*check) : std::string{};
        // Where it goes (backend.h regionDir: the region's folder, else a new one in the install folder).
        const std::string regionKey = check && check->ok() && check->region ? check->region->key : std::string{};
        if (ImGui::GetTime() >= m_nextDestination || regionKey != m_destinationRegion) {
            m_nextDestination = ImGui::GetTime() + 1.0;
            m_destinationRegion = regionKey;
            try {
                m_destination = regionKey.empty() ? m_backend.installRoot() : m_backend.regionDir(m_game, regionKey);
            } catch (...) {
                m_destination = m_backend.installRoot();
            }
            m_destinationFree = freeBytes(m_destination);
            if (m_fixture) {  // (screenshots: the fixtures' player)
                m_destination = path(std::string("/home/player/.local/share/SafeHouse/installations/") + cane::game(m_game).key);
                m_destinationFree = 212ull << 30;
            }
        }
        const bool showDestination = mismatch.empty();
        const float footer = largeHeight() + px(14) + (mismatch.empty() ? 0.0f : px(40) + (canRoute ? controlHeight() : 0.0f)) +
                             (info.empty() ? 0.0f : px(34)) + (showDestination ? controlHeight() + px(8) : 0.0f);
        // A little padding keeps the focus ring of the first and last rows visible.
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - px(6));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(6), px(6) });
        ImGui::BeginChild("iso-list", { w + px(12), ImGui::GetContentRegionAvail().y - footer }, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NavFlattened);
        ImGui::PopStyleVar();
        if (found.empty()) {
            const ImVec2 area = ImGui::GetContentRegionAvail();
            const ImVec2 o = ImGui::GetCursorScreenPos();
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 c{ o.x + area.x * 0.5f, o.y + area.y * 0.42f };
            const char* label = m_scanner.scanning() ? tr("Looking for disc images…") : tr("No disc images found in your usual folders.");
            if (m_scanner.scanning()) spinner(dl, { c.x, c.y - px(26) }, px(14));
            else drawIcon(dl, Icon::Disc, { c.x, c.y - px(26) }, px(34), ink(color::muted));
            const auto t = measure(fonts().body, label);
            dl->AddText(fonts().body, fonts().body->FontSize, { std::round(c.x - t.x * 0.5f), c.y }, ink(color::text2), label);
#if defined(CANE_ANDROID)
            const char* sub = tr("Use Browse… to pick it with Android's file picker.");
#elif defined(CANE_IOS)
            const char* sub = tr("The file picker copies the disc image into the app.");
#else
            const char* sub = padUser() ? tr("Enter the folder that holds your disc image to search it.")
                                        : tr("Use Browse… or drop an .iso or .chd file onto this window.");
#endif
            const auto t2 = measure(fonts().caption, sub);
            dl->AddText(fonts().caption, fonts().caption->FontSize, { std::round(c.x - t2.x * 0.5f), c.y + px(28) }, ink(color::muted), sub);
        }
        for (const auto& candidate : found) {
            const auto id = utf8(candidate.path);
            ImGui::PushID(id.c_str());
            const bool chosen = path(m_iso) == candidate.path;
            const Row r = rowBehavior("##iso", px(60), false, 0.3f);
            if (r.activated && !m_job.busy) m_iso = utf8(candidate.path);
            if (chosen) takeFocus(!input::tracker().lastWasMouse());
            if (r.visible) {
                auto* dl = ImGui::GetWindowDrawList();
                const bool match = matches(candidate);
                if (chosen) {
                    dl->AddRectFilled(r.bb.Min, r.bb.Max, ink(color::accent, 0.1f), px(8));
                    dl->AddRect(r.bb.Min, r.bb.Max, ink(color::accent, 0.6f), px(8), 0, px(1));
                }
                drawIcon(dl, Icon::Disc, { r.bb.Min.x + px(28), r.bb.GetCenter().y }, px(26),
                         ink(chosen ? color::accent : match ? color::text2 : color::muted));
                const float x = r.bb.Min.x + px(54);
                const auto fileName = utf8(candidate.path.filename());
                std::string where = utf8(candidate.path.parent_path());
                if (candidate.size) where += "  ·  " + sizeText(candidate.size);
                // What the image is, on the right.
                const std::string what = discTag(candidate.identity);
                const uint32_t tone = match ? color::success : candidate.identity.kind == DiscIdentity::Kind::Supported ? color::accent : color::muted;
                const ImVec2 ts = tagSize(upper(what).c_str());
                const float tagX = r.bb.Max.x - px(chosen ? 50 : 16) - ts.x;
                drawTag(dl, { tagX, std::round(r.bb.GetCenter().y - ts.y * 0.5f) }, what.c_str(), tone);
                dl->PushClipRect(r.bb.Min, { tagX - px(12), r.bb.Max.y }, true);
                dl->AddText(fonts().body, fonts().body->FontSize, { x, r.bb.Min.y + px(9) }, ink(color::text), fileName.c_str());
                dl->AddText(fonts().caption, fonts().caption->FontSize, { x, r.bb.Min.y + px(33) }, ink(color::muted), where.c_str());
                dl->PopClipRect();
                if (chosen) {
                    const ImVec2 b{ r.bb.Max.x - px(26), r.bb.GetCenter().y };
                    dl->AddCircleFilled(b, px(10), ink(color::accent), 20);
                    drawIcon(dl, Icon::Check, b, px(14), ink(color::bg));
                }
                recordLayout(fileName.c_str(), r.bb);
            }
            ImGui::PopID();
        }
        padScrollHere();
        ImGui::EndChild();
        // A disc image of another game: say what it is, and offer to install
        // it as that game when SafeHouse supports it.
        if (!mismatch.empty()) {
            gap(2);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawIcon(ImGui::GetWindowDrawList(), Icon::Warning, { p.x + px(10), p.y + px(10) }, px(18), ink(color::warning));
            ImGui::SetCursorScreenPos({ p.x + px(28), p.y });
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - px(28));
            text(mismatch.c_str(), fonts().body, color::text);
            ImGui::PopTextWrapPos();
            if (canRoute) {
                const GameId other = *selected->identity.game;
                const std::string label = trf("Install as %s instead", cane::game(other).shortName);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(28));
                if (button(label.c_str(), Tone::Secondary, { 0, 0 }, Icon::ChevronRight) && onRoute) onRoute(other, m_iso);
            }
        }
        // What the chosen disc is and where it goes.
        if (!info.empty()) {
            gap(2);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawIcon(ImGui::GetWindowDrawList(), Icon::Check, { p.x + px(10), p.y + px(10) }, px(18), ink(color::success));
            ImGui::SetCursorScreenPos({ p.x + px(28), p.y });
            text(info.c_str(), fonts().body, color::text);
        }
        // "Installs to <folder> · N GB free   [Change…]"
        if (showDestination) {
            gap(2);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float changeW = std::max(px(130), measure(fonts().body, tr("Change…")).x + px(64));
            const float rowH = controlHeight();
            drawIcon(ImGui::GetWindowDrawList(), Icon::Folder, { p.x + px(10), p.y + rowH * 0.5f }, px(18), ink(color::text2));
            const std::string dest = trf("Installs to %s", utf8(m_destination).c_str()) + " · " + freeText(m_destinationFree);
            ImGui::PushClipRect(p, { p.x + w - changeW - px(12), p.y + rowH }, true);
            ImGui::GetWindowDrawList()->AddText(fonts().body, fonts().body->FontSize,
                                                { p.x + px(28), std::round(p.y + (rowH - fonts().body->FontSize) * 0.5f) }, ink(color::text2),
                                                dest.c_str());
            ImGui::PopClipRect();
            ImGui::SetCursorScreenPos({ p.x + w - changeW, p.y });
            ImGui::BeginDisabled(m_job.busy);
            if (button(tr("Change…###install-location"), Tone::Quiet, { changeW, rowH }, Icon::ChevronRight) && onChangeLocation && !m_fixture)
                onChangeLocation();
            ImGui::EndDisabled();
            ImGui::SetCursorScreenPos({ p.x, p.y + rowH + px(6) });
            ImGui::Dummy({ 1, 0 });
        }
        // Footer: hint and the main action.
        const float ctaW = px(260);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(2));
        const float y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + (largeHeight() - fonts().caption->FontSize) * 0.5f);
#if defined(CANE_ANDROID) || defined(CANE_IOS)
        text(tr("The file picker copies the disc image into the app."), fonts().caption, color::muted);
#else
        text(padUser() ? tr("Not listed? Enter the folder that holds it.") : tr("You can also drop an .iso or .chd file onto this window."), fonts().caption,
             color::muted);
#endif
        ImGui::SetCursorPos({ ImGui::GetCursorPosX() + w - ctaW, y });
        ImGui::BeginDisabled(sessionActive || m_job.busy || !mismatch.empty());
        std::string ctaLabel = ready && !m_addRegion ? tr("Reinstall") : tr("Install");
        bool ctaQuiet = ready && !m_addRegion;
        if (check && check->ok() && check->region) {
            const auto it = std::find_if(m_regions.begin(), m_regions.end(), [&](const auto& r) { return r.region == check->region->key; });
            const std::string region = tr(check->region->name);
            if (it == m_regions.end()) ctaLabel = trf("Install %s", region.c_str()), ctaQuiet = false;
            else if (it->outdated) ctaLabel = trf("Re-extract %s", region.c_str()), ctaQuiet = false;
            else ctaLabel = (it->current ? trf("Reinstall %s", region.c_str()) : trf("Repair %s", region.c_str())), ctaQuiet = it->current;
        }
        ctaLabel += "###install";
        if (button(ctaLabel.c_str(), ctaQuiet ? Tone::Secondary : Tone::Cta, { ctaW, largeHeight() }, Icon::Disc)) install();
        if (m_iso.empty()) takeFocus(!input::tracker().lastWasMouse());
        ImGui::EndDisabled();
        endCard();
    }
    bool matches(const IsoCandidate& c) const {
        return c.identity.kind == DiscIdentity::Kind::Supported && c.identity.game == m_game;
    }
    static std::string discTag(const DiscIdentity& id) {
        switch (id.kind) {
        case DiscIdentity::Kind::Supported: return std::string(cane::game(*id.game).shortName) + " · " + regionOf(id);
        case DiscIdentity::Kind::OtherRegion: return std::string(cane::game(*id.game).shortName) + " · " + regionOf(id) + " · " + tr("not yet");
        case DiscIdentity::Kind::NoPort: return std::string(cane::game(*id.game).shortName) + " · " + tr("no port yet");
        case DiscIdentity::Kind::Unknown: return tr("Other PS2 disc");
        default: return tr("Not a PS2 disc");
        }
    }
    static std::string regionOf(const DiscIdentity& id) {
        if (id.game)
            if (auto r = regionByBootFile(*id.game, id.bootFile)) return tr(r->name);
        return tr("other region");
    }
    bool regionInstalled(const IsoCandidate& c) const {
        if (!c.identity.game) return false;
        const auto r = regionByBootFile(*c.identity.game, c.identity.bootFile);
        return r && std::any_of(m_regions.begin(), m_regions.end(), [&](const auto& in) { return in.region == r->key; });
    }
    // A disc of this game: in "another region" mode one whose region is not installed yet, otherwise one of the
    // selected region (a re-extract), else any.
    std::string bestCandidate() {
        const auto results = m_scanner.results();
        for (const auto& c : results)
            if (matches(c) && (m_addRegion ? !regionInstalled(c) : regionMatchesSelected(c))) return utf8(c.path);
        if (m_addRegion) return {};
        for (const auto& c : results)
            if (matches(c)) return utf8(c.path);
        return {};
    }
    bool regionMatchesSelected(const IsoCandidate& c) const {
        if (m_region.empty() || !c.identity.game) return true;
        const auto r = regionByBootFile(*c.identity.game, c.identity.bootFile);
        return r && r->key == m_region;
    }
    std::string selectedRegionName() const {
        for (const auto& r : m_regions)
            if (r.region == m_region) return r.name();
        return regionName(m_region);
    }
    // The chosen image's check (regions.h), cached by path, size and time: it reads and hashes the executable.
    const DiscCheck& discCheck(const std::string& file) {
        std::error_code ec;
        const auto size = fs::file_size(path(file), ec);
        const auto stamp = fs::last_write_time(path(file), ec);
        if (file != m_checkPath || size != m_checkSize || stamp != m_checkStamp) {
            m_check = checkDisc(path(file), m_game);
            m_checkPath = file, m_checkSize = size, m_checkStamp = stamp;
        }
        return m_check;
    }
    // "Sly 1 · Europe (SCES-509.17) · needs about 12 GB free · installs beside USA"
    std::string discInfo(const DiscCheck& c) const {
        std::string out = c.label();
        if (c.region) {
            if (!c.region->serial.empty()) out += " · " + c.region->serial;
            out += " · " + trf("needs %s free", aboutGb(c.region->needBytes).c_str());
            const bool there = std::any_of(m_regions.begin(), m_regions.end(), [&](const auto& r) { return r.region == c.region->key; });
            if (there) out += " · " + trf("replaces the installed %s files", tr(c.region->name).c_str());
            else if (!m_regions.empty()) out += " · " + trf("installs beside %s", m_regions.size() == 1 ? tr(m_regions[0].name()).c_str() : tr("your other regions"));
        }
        return out;
    }
    LauncherBackend& m_backend;
    Job& m_job;
    IsoScanner m_scanner;
    GameId m_game = GameId::Sly1, m_jobGame = GameId::Sly1;
    bool m_repair = false, m_choseFirst = false, m_fixtureProgress = false, m_outdated = false, m_addRegion = false;
    std::vector<Installation> m_regions;
    std::string m_region;
    DiscCheck m_check;
    std::string m_checkPath;
    uintmax_t m_checkSize = 0;
    fs::file_time_type m_checkStamp{};
    std::string m_note;
    std::string m_iso, m_dismissed, m_lastLabel;
    bool m_scanned = false, m_installAfterSelection = false, m_showLog = false, m_fixture = false, m_fixtureInstalled = false;
    bool m_installed = false, m_pathOpen = false, m_pathFocus = false;
    char m_pathBuf[1024] = {};
    filedialog::Async m_native;
    std::string m_pathNote;
    double m_nextCheck = 0;
    bool m_wasBusy = false;
    fs::path m_destination;
    std::string m_destinationRegion = "-";
    uint64_t m_destinationFree = 0;
    double m_nextDestination = 0;
};
} // namespace cane::ui
