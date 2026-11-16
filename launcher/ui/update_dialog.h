#pragma once
// "Update available": the startup check (Settings > Launcher > Check for updates, on by default) and the manual
// "Check for updates now", the dialog with the new version, how old this build is and the release notes, and the
// download (launcher/update.h). Update now downloads and checks the package, then the launcher closes, its files are
// swapped and the new version starts; while a game or a task runs the restart waits for it.
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include "../backend.h"
#include "../update.h"
#include "shell.h"
#include "widgets.h"

namespace cane::ui {
class UpdateUi {
public:
    enum class Phase { Idle, Checking, Available, UpToDate, CheckFailed, Downloading, Ready, Failed };
    explicit UpdateUi(LauncherBackend& backend) : m_backend(backend), m_install(update::detectInstall()) {}
    bool checkSetting() const { return m_backend.config.value("check_updates", true); }
    void setCheckSetting(bool on) {
        if (on) m_backend.config.erase("check_updates");
        else m_backend.config["check_updates"] = false;
        try {
            m_backend.saveConfig();
        } catch (...) {
        }
    }
    static bool available() { return update::checksAllowed(); }
    // At startup: the result of an update that just finished, the leftovers of the last one, the automatic check.
    void startup(Shell& shell) {
        const auto file = update::resultFile(m_backend.locations.config);
        std::error_code ec;
        if (fs::is_regular_file(file, ec)) {
            try {
                const json r = readJson(file);
                if (r.value("ok", false)) shell.toast(trf("SafeHouse was updated to %s.", update::text(r, "version").c_str()));
                else shell.toast(trf("SafeHouse could not be updated: %s", tr(update::text(r, "message")).c_str()), false);
            } catch (...) {
            }
            fs::remove(file, ec);
        }
        if (available() && checkSetting()) start(false);
        cleanupLater();
    }
    void checkNow() { start(true); }
    // Called every frame: picks up the worker's result. (gameRunning: the automatic check's offer is dropped, so the
    // dialog does not greet the player when the game quits; the next start of the launcher offers it again.)
    void poll(Shell& shell, bool gameRunning = false) {
        if (m_fixture) return;
        std::lock_guard lock(m_shared->mutex);
        const Phase phase = m_shared->phase;
        if (phase == m_seen) return;
        const Phase before = m_seen;
        m_seen = phase;
        if (phase == Phase::Available && before == Phase::Downloading) {
            // (the download was cancelled: the offer stays, without the dialog)
        } else if (phase == Phase::Available) {
            m_release = m_shared->release;
            m_package = update::packageFor(m_release, m_install);
            const bool skipped = m_backend.config.value("skipped_update", std::string{}) == m_release.tag;
            if (m_manual || (!skipped && !gameRunning)) m_openDialog = true;
        } else if (phase == Phase::UpToDate && m_manual) shell.toast(trf("SafeHouse is up to date (%s).", update::describe(update::current()).c_str()));
        else if (phase == Phase::CheckFailed && m_manual) shell.toast(trf("Could not check for updates: %s", tr(m_shared->error).c_str()), false);
        else if (phase == Phase::Ready) m_plan = m_shared->plan;
        else if (phase == Phase::Failed) {
            m_error = m_shared->error;
            m_openDialog = true;
        }
    }
    Phase phase() const { return m_seen; }
    const update::Release& release() const { return m_release; }
    // The update is downloaded and nothing runs: time to close the launcher and swap the files.
    bool restartNow(bool busy) const { return m_seen == Phase::Ready && m_restart && !busy; }
    // The update to finish when the launcher closes (restart: start the new version afterwards).
    std::optional<update::Plan> pendingPlan() const { return m_seen == Phase::Ready ? m_plan : std::nullopt; }
    bool restartAfterApply() const { return m_restart; }

    // Settings > Launcher.
    template <class Describe> void settingsRows(Describe describe, bool fixture) {
        if (!available() && !fixture) {
            linkRow(tr("Version"), update::describe(update::current()).c_str(), nullptr, Icon::None);
            describe(tr("A development build: it does not check for updates."));
            return;
        }
        bool on = checkSetting();
        if (toggleRow(tr("Check for updates"), &on) && !fixture) setCheckSetting(on);
        describe(tr("When the launcher starts, asks GitHub whether a newer SafeHouse exists. Only the launcher's version is sent."));
        const bool checking = m_seen == Phase::Checking;
        m_versionText = checking ? tr("Checking…") : update::describe(update::current());
        if (linkRow(tr("Check for updates now"), m_versionText.c_str(), nullptr, Icon::Refresh) && !checking && !fixture) checkNow();
        describe(tr("Looks for a newer version now. Your version is shown on the right."));
        if (m_seen == Phase::Available || m_seen == Phase::Downloading || m_seen == Phase::Ready) {
            m_availableText = m_release.version;
            if (linkRow(tr("Update available"), m_availableText.c_str(), nullptr, Icon::ChevronRight)) m_openDialog = true;
            describe(tr("Shows what is new and updates SafeHouse."));
        }
    }

    // Screenshots (--screenshot ... --screen update / update-download / update-ready).
    void fixture(const std::string& screen) {
        m_release.tag = "v0.1.57";
        m_release.version = "0.1.57";
        m_release.date = "2026-11-17T09:12:44Z";
        m_release.notes = "## Changes\n\n- Sly 2: the Contessa's searchlights cast shadows again\n- Sly 1: the camera no longer clips "
                          "through Mz. Ruby's swamp huts\n- Launcher: Check for updates (Settings > Launcher) and seamless updates\n"
                          "- Windows: the setup shows the installed version in Apps & features\n- Sly 3: the Venice hub loads (in development)\n\n"
                          "| For | File |\n|---|---|\n| Linux | `SafeHouse-Launcher-Linux-x86_64.AppImage` |\n";
        m_fixtureBuild = {"0.1.45", "d308441e", "2026-11-05T19:11:09Z"};
        m_package = update::Package{"linux-appimage", "SafeHouse-Launcher-Linux-x86_64.AppImage", std::string(64, '0'), "fixture", 187654321};
        m_seen = screen == "update-download" ? Phase::Downloading : screen == "update-ready" ? Phase::Ready
                 : screen == "update-failed" ? Phase::Failed : Phase::Available;
        m_shared->done = 112592592;
        m_shared->total = 187654321;
        m_error = tr("The download is damaged (its SHA-256 does not match the release). Nothing was changed.");
        m_fixture = true;
        m_openDialog = true;
    }

    void drawDialog(bool busy) {
        if (m_openDialog) {
            ImGui::OpenPopup("Update##dialog");
            m_openDialog = false;
        }
        ImGui::SetNextWindowSize({ px(620), 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(26), px(24) });
        if (ImGui::BeginPopupModal("Update##dialog", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            const bool back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
            switch (m_seen) {
            case Phase::Downloading: downloading(back); break;
            case Phase::Ready: ready(busy, back); break;
            case Phase::Failed: failed(back); break;
            case Phase::Available: offer(back); break;
            default: ImGui::CloseCurrentPopup(); break;
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

private:
    struct Shared {
        std::mutex mutex;
        Phase phase = Phase::Idle;
        update::Release release;
        update::Plan plan;
        std::string error;
        std::atomic<uint64_t> done{ 0 }, total{ 0 };
        std::atomic<bool> cancel{ false };
    };
    update::BuildInfo build() const { return m_fixture ? m_fixtureBuild : update::current(); }
    void start(bool manual) {
        {
            std::lock_guard lock(m_shared->mutex);
            if (m_shared->phase == Phase::Checking || m_shared->phase == Phase::Downloading || m_shared->phase == Phase::Ready) return;
            m_shared->phase = Phase::Checking;
        }
        m_manual = manual;
        m_seen = Phase::Checking;
        // The worker owns a reference to the shared state: it may outlive the window (an offline check waits for its
        // timeout), and the launcher never waits for it on exit.
        std::thread([shared = m_shared, install = m_install, manual] {
            // (the automatic check does not wait for the cleanup: cleanupLater)
            if (manual) {
                std::lock_guard lock(shared->mutex);
                if (shared->phase == Phase::Checking) update::cleanup(install);
            }
            Phase result = Phase::CheckFailed;
            update::Release release;
            std::string error;
            try {
                release = update::fetchLatest(&shared->cancel);
                result = update::compareVersions(release.version, update::current().version) > 0 ? Phase::Available : Phase::UpToDate;
            } catch (const std::exception& e) {
                error = e.what();
            }
            std::lock_guard lock(shared->mutex);
            shared->release = std::move(release);
            shared->error = error;
            shared->phase = result;
        }).detach();
    }
    void cleanupLater() {
        // (later: the helper of the update that started this launcher may still be exiting)
        std::thread([shared = m_shared, install = m_install] {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::lock_guard lock(shared->mutex);
            if (shared->phase != Phase::Downloading && shared->phase != Phase::Ready) update::cleanup(install);
        }).detach();
    }
    void download() {
        {
            std::lock_guard lock(m_shared->mutex);
            m_shared->phase = Phase::Downloading;
            m_shared->done = 0;
            m_shared->total = m_package ? m_package->size : 0;
            m_shared->cancel = false;
        }
        m_seen = Phase::Downloading;
        m_restart = true;
        std::thread([shared = m_shared, install = m_install, release = m_release] {
            Phase result = Phase::Failed;
            update::Plan plan;
            std::string error;
            try {
                plan = update::stage(release, install, [&](uint64_t done, uint64_t total) {
                    shared->done = done;
                    if (total) shared->total = total;
                }, &shared->cancel, [](const std::string&) {});
                result = Phase::Ready;
            } catch (const std::exception& e) {
                error = e.what();
                if (shared->cancel) result = Phase::Available;
                update::cleanup(install);
            }
            std::lock_guard lock(shared->mutex);
            shared->plan = plan;
            shared->error = error;
            shared->phase = result;
        }).detach();
    }
    static std::string megabytes(uint64_t bytes) {
        char text[32];
        std::snprintf(text, sizeof text, tr("%.1f MB"), static_cast<double>(bytes) / 1e6);
        return text;
    }
    // GitHub's release notes (Markdown) as plain lines: headings and bullets kept, tables and markup left out.
    static std::string plainNotes(const std::string& notes) {
        std::string out, line;
        std::istringstream in(notes);
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] == '|') continue;
            size_t hashes = 0;
            while (hashes < line.size() && line[hashes] == '#') ++hashes;
            if (hashes && hashes < line.size() && line[hashes] == ' ') line = line.substr(hashes + 1);
            else if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) line = "•  " + line.substr(2);
            std::string clean;
            for (size_t i = 0; i < line.size(); ++i) {
                if (line[i] == '`' || (line[i] == '*' && i + 1 < line.size() && line[i + 1] == '*')) {
                    if (line[i] == '*') ++i;
                    continue;
                }
                clean += line[i];
            }
            if (clean.empty() && (out.empty() || out.ends_with("\n\n"))) continue;
            out += clean + "\n";
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        return out.empty() ? tr("No release notes.") : out;
    }
    void header(const char* title) {
        heading(title);
        const auto b = build();
        const int days = update::daysBehind(b.date, m_release.date);
        std::string line = (m_release.date.size() >= 10 ? trf("Released %s.", m_release.date.substr(0, 10).c_str()) : std::string(tr("Released recently."))) + " " +
                           trf("You have %s.", update::describe(b).c_str());
        textWrapped(line.c_str(), fonts().body, color::text2);
        if (days > 0) {
            const std::string old = days == 1 ? std::string(tr("Your build is 1 day old.")) : trf("Your build is %d days old.", days);
            textWrapped(old.c_str(), fonts().body, days >= 30 ? color::warning : color::text);
        }
    }
    void buttonsRow(std::initializer_list<std::pair<const char*, Tone>> buttons, int* pressed) {
        const float n = static_cast<float>(buttons.size());
        const float w = std::floor((ImGui::GetContentRegionAvail().x - px(12) * (n - 1)) / n);
        int i = 0;
        for (const auto& [label, tone] : buttons) {
            if (i) ImGui::SameLine(0, px(12));
            if (button(label, tone, { w, 0 })) *pressed = i;
            if (i == static_cast<int>(n) - 1) {
                if (ImGui::IsWindowAppearing()) setFocus(GImGui->LastItemData.ID, ImGui::GetCurrentWindow(), !input::tracker().lastWasMouse());
            }
            recordLayout(label, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
            ++i;
        }
    }
    void offer(bool back) {
        const std::string title = trf("SafeHouse %s is available", m_release.version.c_str());
        header(title.c_str());
        gap(6);
        overline(tr("WHAT'S NEW"));
        gap(2);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(color::bg));
        ImGui::BeginChild("update-notes", { 0, px(200) }, ImGuiChildFlags_Border);
        {
            FontScope f(fonts().caption);
            ColorScope c(color::text2);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(plainNotes(m_release.notes).c_str());
            ImGui::PopTextWrapPos();
            padScrollHere();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        recordLayout("update-notes", ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
        gap(4);
        const bool can = m_package.has_value();
        if (can) {
            const std::string what = trf("Downloads %s (%s) and restarts SafeHouse. Your games, saves and settings are not changed.",
                                         m_package->file.c_str(), megabytes(m_package->size).c_str());
            caption(what.c_str(), color::muted);
        } else {
            caption((m_install.reason.empty() ? std::string(tr("This release has no package that can update this copy of SafeHouse."))
                                              : tr(m_install.reason)).c_str(), color::warning);
        }
        gap(8);
        int pressed = -1;
        buttonsRow({ { tr("Skip this version"), Tone::Quiet }, { tr("Later"), Tone::Secondary }, { can ? tr("Update now") : tr("Open download page"), Tone::Cta } }, &pressed);
        if (pressed == 0) {
            if (!m_fixture) {
                m_backend.config["skipped_update"] = m_release.tag;
                try {
                    m_backend.saveConfig();
                } catch (...) {
                }
            }
            ImGui::CloseCurrentPopup();
        } else if (pressed == 1 || back) ImGui::CloseCurrentPopup();
        else if (pressed == 2 && !m_fixture) {
            if (can) download();
            else openPage();
            if (!can) ImGui::CloseCurrentPopup();
        }
    }
    void downloading(bool back) {
        const std::string title = trf("Updating to SafeHouse %s", m_release.version.c_str());
        heading(title.c_str());
        textWrapped(tr("Downloading and checking the new version. SafeHouse restarts when it is ready; your games, saves and settings are not changed."),
                    fonts().body, color::text2);
        gap(10);
        const uint64_t done = m_shared->done, total = m_shared->total;
        const float fraction = total ? std::clamp(static_cast<float>(done) / static_cast<float>(total), 0.0f, 1.0f) : 0.0f;
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        dl->AddRectFilled(p, { p.x + w, p.y + px(8) }, ink(color::raised), px(4));
        dl->AddRectFilled(p, { p.x + w * fraction, p.y + px(8) }, ink(color::cta), px(4));
        recordLayout("update-progress", ImRect(p, { p.x + w, p.y + px(8) }));
        ImGui::Dummy({ w, px(8) });
        gap(4);
        const std::string amount = total ? trf("%s of %s", megabytes(done).c_str(), megabytes(total).c_str()) : megabytes(done);
        caption(amount.c_str(), color::text2);
        gap(8);
        int pressed = -1;
        buttonsRow({ { tr("Cancel"), Tone::Secondary } }, &pressed);
        if ((pressed == 0 || back) && !m_fixture) {
            m_shared->cancel = true;
            ImGui::CloseCurrentPopup();
        }
    }
    void ready(bool busy, bool back) {
        const std::string title = trf("SafeHouse %s is ready", m_release.version.c_str());
        heading(title.c_str());
        textWrapped(busy || m_fixture ? tr("SafeHouse restarts to finish the update as soon as the game or the current task ends.")
                                      : tr("Restarting SafeHouse…"),
                    fonts().body, color::text2);
        gap(8);
        int pressed = -1;
        buttonsRow({ { tr("Update when I close SafeHouse"), Tone::Secondary } }, &pressed);
        if ((pressed == 0 || back) && !m_fixture) {
            m_restart = false;
            ImGui::CloseCurrentPopup();
        }
    }
    void failed(bool back) {
        heading(tr("SafeHouse could not be updated"));
        textWrapped(m_error.c_str(), fonts().body, color::warning);
        gap(4);
        caption(tr("You can also download the new version from the releases page."), color::text2);
        gap(8);
        int pressed = -1;
        buttonsRow({ { tr("Close"), Tone::Secondary }, { tr("Open download page"), Tone::Primary } }, &pressed);
        if (pressed == 1 && !m_fixture) openPage();
        if ((pressed >= 0 || back) && !m_fixture) {
            std::lock_guard lock(m_shared->mutex);
            m_shared->phase = Phase::Available;
            m_seen = Phase::Available;
            ImGui::CloseCurrentPopup();
        }
    }
    void openPage() {
        try {
            openFolder(path(m_release.page.empty() ? update::releasesPage : m_release.page));
        } catch (...) {
        }
    }

    LauncherBackend& m_backend;
    update::Install m_install;
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    Phase m_seen = Phase::Idle;
    update::Release m_release;
    std::optional<update::Package> m_package;
    std::optional<update::Plan> m_plan;
    update::BuildInfo m_fixtureBuild;
    std::string m_error, m_versionText, m_availableText;
    bool m_manual = false, m_openDialog = false, m_restart = true, m_fixture = false;
};
} // namespace cane::ui
