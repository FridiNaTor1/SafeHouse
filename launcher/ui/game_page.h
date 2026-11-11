#pragma once
// The page of the game selected in the rail: a large logo over a tinted
// backdrop, the title and state on the left, and the main action (Play,
// Install, or "In development") with the secondary actions on the right.
#include <array>
#include <functional>
#include <string>
#include <vector>
#include "../backend.h"
#include "../desktop_shortcut.h"
#include "../steam.h"
#include "games.h"
#include "job.h"
#include "prompt_bar.h"
#include "shell.h"
#include "widgets.h"

namespace cane::ui {
// What the launcher knows about one game's installation.
struct GameStatus {
    bool assets = false;  // current installation manifest
    bool runtime = false; // runtime program present
    bool repair = false;  // an old or damaged installation
    std::string iso;      // disc image it was installed from
    bool inSteam = false; // its "Add to Steam" shortcut is in every Steam account
    bool onDesktop = false; // its desktop shortcut exists (desktop_shortcut.h)
    bool outdated = false;           // valid, but an update changed the asset format: re-extract
    bool texturesMissing = false;    // Sly 2 installed before the texture dump came with the install (backend.h)
    std::vector<Installation> regions; // every installed region (backend.h)
    std::string region;              // the selected one (regions[].region)
    // "Sly 1 (Europe)" style suffix for texts, "" with one region
    std::string regionName() const {
        for (const auto& r : regions)
            if (r.region == region) return r.name();
        return {};
    }
};

class GamePage {
public:
    // Actions main.cpp provides.
    // addRegion: "Add region", the install page in its "Install another region" mode (as Settings > Games).
    std::function<void(GameId)> openInstall, addRegion;
    // Re-extract the selected region from this disc image now (main.cpp: the install page, started).
    std::function<void(GameId, std::string)> reextract;
    // The game the running job plays, or -1.
    int playingGame = -1;
    // Set while an install of this game runs, for the progress shortcut.
    int installingGame = -1;

    GamePage(LauncherBackend& backend, Job& job) : m_backend(backend), m_job(job) {}
    void useFixture(const std::array<GameStatus, kGameCount>& statuses) {
        m_fixture = true;
        m_status = statuses;
        // Screenshots show "Add to Steam" enabled only with a fake Steam tree (SAFEHOUSE_STEAM_ROOT), never the real one.
        if (!steam::detail::env("SAFEHOUSE_STEAM_ROOT").empty()) try {
                m_steam = steam::find();
            } catch (...) {
            }
    }
    // Re-reads the installation manifests (once a second, and when a job ends).
    const GameStatus& status(GameId game) {
        refresh();
        return m_status[static_cast<size_t>(game)];
    }
    void invalidate() { m_nextCheck = 0; }
    // Screenshots: open the re-extract prompt.
    void showReextract(GameId game) { m_confirmReextract = static_cast<int>(game); }
    // Screenshots: the desktop shortcut dialog, or its result.
    void showDesktopShortcut(GameId game, bool done) {
        if (!done) {
            m_confirmShortcut = static_cast<int>(game);
            return;
        }
        m_shortcutGame = static_cast<int>(game);
        m_shortcutOk = true;
        m_shortcutHeading = tr("Desktop shortcut added");
        try {
            m_shortcutMessage = trf("%s has a shortcut on your desktop now:", cane::game(game).title) + "\n" +
                                shortcut::displayPath(shortcut::file(game, shortcut::Where::Desktop)) + "\n\n" +
                                trf("and in the %s:", tr(shortcut::whereName(shortcut::Where::Menu))) + "\n" + shortcut::displayPath(shortcut::file(game, shortcut::Where::Menu));
        } catch (...) {
        }
        m_showShortcutResult = true;
    }
    // Screenshots: the Sly 3 notice shown before Play.
    void showSly3Notice() { m_confirmSly3 = true; }
    // Start: the page's main action.
    void primaryAction(GameId game) {
        const auto& s = status(game);
        if (m_job.busy) return;
        if (s.assets && s.runtime) play(game);
        else if (s.outdated) m_confirmReextract = static_cast<int>(game);
        else if (installable(game) && openInstall) openInstall(game);
    }
    std::vector<launcher::Hint> hints(GameId game) {
        using input::Button;
        std::vector<launcher::Hint> h{ { Button::LB, 0, "" }, { Button::RB, 0, N_("Games") } };
        const auto& s = status(game);
        if (!m_job.busy && s.assets && s.runtime) h.push_back({ Button::Start, 0, N_("Play") });
        else if (!m_job.busy && installable(game))
            h.push_back({ Button::Start, 0, s.outdated ? N_("Re-extract") : s.repair ? N_("Repair") : s.assets ? N_("Reinstall") : N_("Install") });
        h.push_back({ backButton(), GLFW_KEY_ESCAPE, N_("Back") });
        h.push_back({ confirmButton(), GLFW_KEY_ENTER, N_("Select") });
        return h;
    }

    void draw(GameId id, ImVec2 size) {
        const GameInfo& g = game(id);
        const GameStatus& s = status(id);
        const bool busy = m_job.busy.load();
        const bool playing = busy && playingGame == static_cast<int>(id);
        const bool installing = busy && installingGame == static_cast<int>(id);
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetWindowPos();
        // Backdrop: the game's tint glowing behind the logo, fading to the
        // page colour at the bottom where the text sits.
        dl->AddRectFilledMultiColor(o, { o.x + size.x, o.y + size.y }, u32(mix(color::bg, g.accent, 0.20f)), u32(mix(color::bg, g.accent, 0.12f)),
                                    u32(color::bg), u32(color::bg));
        const Image& logo = images().games[static_cast<int>(id)];
        const bool narrow = size.x < px(760);
        const float margin = std::round(std::clamp(size.x * 0.045f, px(24), px(56)));
        const float actionsW = narrow ? std::min(size.x - margin * 2, px(420)) : px(340);
        const bool hasActions = installable(id);
        // (Sly 2 runs the native port; the emulated runtime's renderer preview toggle is gone)
        const bool showNativePreview = false;
        const char* previewHelp = tr("Experimental. GS compatibility stays active; F8 switches views.");
        const float previewHelpH = fonts().caption->CalcTextSizeA(fonts().caption->FontSize, FLT_MAX,
                                                                 actionsW - px(32), previewHelp).y;
        const float previewH = px(48) + previewHelpH + px(12);
        // Bottom block heights decide how much room the logo gets.
        const bool regionRow = hasActions && s.regions.size() > 1 && !playing && !installing;
        const float actionsH = !hasActions ? 0.0f : px(58) + (regionRow ? controlHeight() + px(10) : 0.0f) + (secondaryCount(id, s, busy) ? gridHeight(s, installing) : 0.0f)
                                                          + (steamRow(s) ? px(4) + px(30) + (shortcutRow(id, s) && !shortcutBesideSteam(s, actionsW) ? px(4) + px(30) : 0.0f) : 0.0f)
                                                          + (showNativePreview ? previewH + px(8) : 0.0f);
        const float infoH = px(150);
        const float blockH = narrow ? infoH + (hasActions ? actionsH + px(20) : 0.0f) : std::max(infoH, actionsH);
        // Sly 3: the early-development banner across the top of the page.
        const float bannerH = id == GameId::Sly3 ? earlyBanner({ o.x + margin, o.y + margin }, size.x - margin * 2) + px(12) : 0.0f;
        const float heroTop = margin + bannerH, heroBottom = size.y - margin - blockH - px(24);
        if (logo.texture && heroBottom > heroTop + px(60)) {
            const float maxW = std::min(size.x * 0.6f, px(760)), maxH = (heroBottom - heroTop) * 0.92f;
            float lw = maxW, lh = lw / logo.aspect;
            if (lh > maxH) lh = maxH, lw = lh * logo.aspect;
            const ImVec2 c{ o.x + size.x * 0.5f, o.y + (heroTop + heroBottom) * 0.5f };
            drawGlow(dl, c, lw * 0.78f, lh * 0.82f, g.accent, 0.38f);
            dl->AddImage(logo.texture, { std::round(c.x - lw * 0.5f), std::round(c.y - lh * 0.5f) },
                         { std::round(c.x + lw * 0.5f), std::round(c.y + lh * 0.5f) });
            recordLayout("hero-logo", ImRect({ c.x - lw * 0.5f, c.y - lh * 0.5f }, { c.x + lw * 0.5f, c.y + lh * 0.5f }));
        }
        // Title block.
        const float blockTop = size.y - margin - blockH;
        const float infoW = narrow || !hasActions ? size.x - margin * 2 : size.x - margin * 2 - actionsW - px(40);
        ImGui::SetCursorPos({ margin, blockTop + (narrow ? 0.0f : std::max(0.0f, blockH - infoH)) });
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + infoW);
        {
            std::string pill;
            uint32_t tone = color::muted;
            if (playing) pill = tr("Playing"), tone = color::success;
            else if (installing) pill = tr("Installing"), tone = color::accent;
            else if (!installable(id)) pill = tr("In development"), tone = color::accent;
            else if (s.outdated) pill = tr("Update needed"), tone = color::warning;
            else if (s.repair) pill = tr("Needs repair"), tone = color::warning;
            else if (s.assets && s.runtime) pill = tr("Ready to play"), tone = color::success;
            else if (s.assets) pill = tr("Installed"), tone = color::success;
            else pill = tr("Not installed"), tone = color::muted;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawPill(dl, p, pill.c_str(), tone);
            float x = p.x + pillSize(pill.c_str()).x + px(8);
            if (s.assets && !s.runtime && installable(id)) {
                const char* dev = tr("Port in development");
                drawPill(dl, { x, p.y }, dev, color::accent);
                x += pillSize(dev).x + px(8);
            }
            // The region, when one is installed (the dropdown picks another).
            if (!s.regions.empty() && installable(id)) drawPill(dl, { x, p.y }, s.regionName().c_str(), color::muted);
            ImGui::Dummy({ 1, pillSize("x").y });
            gap(2);
            ImFont* titleFont = measure(fonts().display, g.title).x <= infoW ? fonts().display : fonts().title;
            wrapped(g.title, titleFont, color::text);
            tighten(6);
            wrapped(tr(g.blurb), fonts().body, color::text2);
            tighten(4);
            wrapped(detail(id, s, installing).c_str(), fonts().caption, color::muted);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        if (!hasActions) return;
        // Actions.
        const float ax = narrow ? margin : size.x - margin - actionsW;
        const float ay = narrow ? size.y - margin - actionsH : blockTop + std::max(0.0f, blockH - actionsH);
        ImGui::SetCursorPos({ ax, ay });
        ImGui::BeginGroup();
        const ImVec2 cta{ actionsW, px(58) };
        if (regionRow) regionPicker(id, s, actionsW, busy);
        if (playing) {
            if (button(tr("Stop game"), Tone::Danger, cta, Icon::Stop)) m_job.cancel = true;
            takeFocus(!input::tracker().lastWasMouse());
        } else if (installing) {
            progressButton(cta);
        } else if (s.assets && !s.runtime) {
            badge(tr("In development"), cta);
        } else {
            ImGui::BeginDisabled(busy);
            const bool ready = s.assets && s.runtime;
            const char* label = ready ? tr("Play") : s.outdated ? tr("Re-extract") : s.repair ? tr("Repair") : tr("Install");
            if (bigButton(label, cta, ready ? Icon::Play : s.outdated ? Icon::Refresh : Icon::Disc)) {
                if (ready) play(id);
                else if (s.outdated) m_confirmReextract = static_cast<int>(id);
                else if (openInstall) openInstall(id);
            }
            takeFocus(!input::tracker().lastWasMouse());
            ImGui::EndDisabled();
        }
        if (showNativePreview) {
            gap(8);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
            ImGui::BeginChild("native-preview-options", {actionsW, previewH}, ImGuiChildFlags_None,
                              ImGuiWindowFlags_NavFlattened | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            ImGui::BeginDisabled(busy);
            bool enabled = m_backend.sly2NativePreviewEnabled();
            if (toggleRow(tr("Native renderer preview"), &enabled, nullptr, px(48)) && !m_fixture) {
                try {
                    m_backend.setSly2NativePreviewEnabled(enabled);
                } catch (const std::exception& e) {
                    m_previewError = e.what();
                    m_showPreviewError = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::SetCursorPosX(px(16));
            ImGui::PushTextWrapPos(actionsW - px(16));
            wrapped(previewHelp, fonts().caption, color::muted);
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
        }
        if (secondaryCount(id, s, busy)) {
            gap(0);
            const float half = std::floor((actionsW - px(8)) * 0.5f);
            ImGui::BeginDisabled(busy);
            int n = 0;
            const auto next = [&] {
                if (n++ % 2) ImGui::SameLine(0, px(8));
            };
            if (installing) {
                next();
                ImGui::EndDisabled();
                if (button(tr("Show progress"), Tone::Secondary, { half, 0 }, Icon::ChevronRight) && openInstall) openInstall(id);
                takeFocus(!input::tracker().lastWasMouse());
                ImGui::BeginDisabled(busy);
            }
            if (s.assets && !installing) {
                next();
                if (button(tr("Verify files"), Tone::Secondary, { half, 0 }, Icon::Check)) verify(id);
                takeFocus(!input::tracker().lastWasMouse());
                next();
                if (button(tr("Reinstall"), Tone::Secondary, { half, 0 }, Icon::Disc) && openInstall) openInstall(id);
                // Another region / release beside this one (the Region menu above Play then switches between them).
                next();
                if (button(tr("Add region"), Tone::Secondary, { half, 0 }, Icon::Plus) && addRegion) addRegion(id);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("%s", trf("Install another region of %s (%s) beside this one.\n"
                                                "Your installed regions stay; pick one with the Region menu above Play.\n"
                                                "Saves and settings are shared by the regions.", game(id).shortName, supportedRegionList(id).c_str()).c_str());
                next();
                if (button(tr("Uninstall"), Tone::Secondary, { half, 0 }, Icon::Trash)) m_confirmUninstall = static_cast<int>(id);
                // (Sly 2 installed before 2026-11-04: its texture dump, a third row of the grid; gridRows)
                if (s.texturesMissing) {
                    next();
                    if (button(tr("Export textures"), Tone::Secondary, { half, 0 }, Icon::Folder)) exportTextures(id);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                        ImGui::SetTooltip("%s", tr("Write every texture of the game as a PNG into the game folder's Textures folder,\n"
                                          "for texture mods (about 80 MB, under a minute). Newer installs have it already."));
                }
            }
            ImGui::EndDisabled();
            if (!installing) {
                gap(0);
                if (button(tr("Open save folder"), Tone::Quiet, { 0, px(30) }, Icon::Folder)) openFolderSafe(m_backend.userDir(id));
                if (s.assets || s.repair) {
                    ImGui::SameLine(0, px(4));
                    if (button(tr("Open game folder"), Tone::Quiet, { 0, px(30) }, Icon::Folder)) openFolderSafe(m_backend.dataDir(id));
                }
            }
        }
#ifdef CANE_ANDROID
        // Android: no Steam; "Add shortcut" asks the home screen to pin one that starts the game directly
        // (android_app.h pinShortcut: the direct activity runs `--play GAME`, the app closes when the game quits).
        if (steamRow(s) && !installing) {
            gap(0);
            ImGui::BeginDisabled(busy);
            if (button(tr("Add shortcut"), Tone::Quiet, { 0, px(30) }, Icon::Plus) && !m_fixture) pinShortcut(id);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("%s", tr("Put a shortcut on your home screen that starts this game directly"));
        }
        if ((false)) {
#else
        if (steamRow(s) && !installing) {
#endif
            gap(0);
            // Disabled without Steam; the tooltip says why (also over the disabled button).
            ImGui::BeginDisabled(busy || !m_steam);
            if (button(s.inSteam ? tr("Update Steam shortcut") : tr("Add to Steam"), Tone::Quiet, { 0, px(30) }, s.inSteam ? Icon::Refresh : Icon::Plus))
                m_confirmSteam = static_cast<int>(id);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("%s", !m_steam ? tr("Steam was not found on this computer")
                                       : s.inSteam ? tr("The shortcut is in Steam. Update it (for example after moving SafeHouse)")
                                                   : tr("Add a Steam shortcut that starts this game directly, with its artwork"));
            // "Add desktop shortcut" (Sly 1, Sly 2): beside "Add to Steam" when both fit, else under it.
            if (shortcutRow(id, s)) {
                if (shortcutBesideSteam(s, actionsW)) ImGui::SameLine(0, px(4));
                ImGui::BeginDisabled(busy);
                if (button(tr(shortcutLabel(s)), Tone::Quiet, { 0, px(30) }, s.onDesktop ? Icon::Refresh : Icon::Plus))
                    m_confirmShortcut = static_cast<int>(id);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("%s", s.onDesktop ? tr("The shortcut is on your desktop. Update it (for example after moving SafeHouse)")
                                                        : tr("Put a shortcut that starts this game directly on your desktop"));
            }
        }
        ImGui::EndGroup();
    }
    void drawSteamDialogs() {
        if (m_confirmSteam >= 0) {
            ImGui::OpenPopup("Steam##confirm");
            m_steamGame = m_confirmSteam;
            m_steamRunning = !m_fixture && m_steam && steam::running(*m_steam);
            m_confirmSteam = -1;
        }
        ImGui::SetNextWindowSize({ px(520), 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(24), px(22) });
        if (ImGui::BeginPopupModal("Steam##confirm", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            const GameId id = static_cast<GameId>(std::clamp(m_steamGame, 0, kGameCount - 1));
            heading(trf("Add %s to Steam?", game(id).shortName).c_str());
            const int accounts = m_steam ? static_cast<int>(m_steam->accounts.size()) : 0;
            textWrapped((accounts == 1 ? trf("SafeHouse adds a shortcut that starts %s directly, with its library artwork, to your Steam account. "
                                             "Steam's shortcut list is backed up first.", game(id).title)
                                       : trf("SafeHouse adds a shortcut that starts %s directly, with its library artwork, to %d Steam accounts. "
                                             "Steam's shortcut list is backed up first.", game(id).title, accounts)).c_str(), fonts().body, color::text2);
            if (m_status[static_cast<size_t>(id)].regions.size() > 1) {
                gap(4);
                textWrapped(trf("The shortcut starts the region selected here, now %s. Pick another region in SafeHouse and Steam starts that one.",
                                tr(m_status[static_cast<size_t>(id)].regionName()).c_str()).c_str(), fonts().body, color::text2);
            }
            if (m_steamRunning) {
                gap(4);
                textWrapped(tr("Steam is running. It only reads shortcuts when it starts, and it can write its own list back over the new "
                            "shortcut. Exit Steam first for the safest result, or restart Steam after adding."),
                            fonts().body, color::warning);
            }
            gap(8);
            const float w = std::floor((ImGui::GetContentRegionAvail().x - px(12)) * 0.5f);
            if (button(tr("Cancel"), Tone::Secondary, { w, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            takeFocusNow();
            ImGui::SameLine(0, px(12));
            if (button(tr("Add to Steam"), Tone::Primary, { w, 0 }, Icon::Plus)) {
                ImGui::CloseCurrentPopup();
                if (!m_fixture && m_steam) {
                    try {
                        const auto r = steam::add(*m_steam, id);
                        m_steamMessage = (r.addedEntries ? (r.accounts == 1 ? trf("%s was added to your Steam library.", game(id).title)
                                                                            : trf("%s was added to %d Steam libraries.", game(id).title, r.accounts))
                                                         : (r.accounts == 1 ? trf("%s is up to date in your Steam library.", game(id).title)
                                                                            : trf("%s is up to date in %d Steam libraries.", game(id).title, r.accounts)))
                                         + " " + (steam::running(*m_steam) ? tr("Restart Steam to see it.") : tr("It shows the next time Steam starts."));
                        m_steamOk = true;
                    } catch (const std::exception& e) {
                        m_steamMessage = trf("Could not add the shortcut: %s. Steam's files were not changed.", tr(std::string(e.what())).c_str());
                        m_steamOk = false;
                    }
                    m_showSteamResult = true;
                    invalidate();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        if (m_showSteamResult) {
            ImGui::OpenPopup("Steam##result");
            m_showSteamResult = false;
        }
        ImGui::SetNextWindowSize({ px(480), 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(24), px(22) });
        if (ImGui::BeginPopupModal("Steam##result", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            heading(m_steamOk ? tr("Added to Steam") : tr("Steam shortcut not added"));
            textWrapped(m_steamMessage.c_str(), fonts().body, color::text2);
            gap(8);
            if (button(tr("Close"), Tone::Secondary, { 0, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            takeFocusNow();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    void drawShortcutDialogs() {
        if (m_confirmShortcut >= 0) {
            ImGui::OpenPopup("Shortcut##confirm");
            m_shortcutGame = m_confirmShortcut;
            m_confirmShortcut = -1;
            const GameId id = static_cast<GameId>(std::clamp(m_shortcutGame, 0, kGameCount - 1));
            m_shortcutExists = shortcut::exists(id, shortcut::Where::Desktop);
            m_shortcutMenu = shortcut::exists(id, shortcut::Where::Menu) || m_fixture;
            try {
                m_shortcutFolder = shortcut::displayPath(shortcut::folder(shortcut::Where::Desktop));
            } catch (const std::exception& e) {
                m_shortcutFolder.clear();
            }
        }
        ImGui::SetNextWindowSize({ px(540), 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(24), px(22) });
        if (ImGui::BeginPopupModal("Shortcut##confirm", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            const GameId id = static_cast<GameId>(std::clamp(m_shortcutGame, 0, kGameCount - 1));
            const auto& st = m_status[static_cast<size_t>(id)];
            heading((m_shortcutExists ? trf("Update the desktop shortcut for %s?", game(id).shortName) : trf("Add a desktop shortcut for %s?", game(id).shortName)).c_str());
            textWrapped((m_shortcutExists ? trf("SafeHouse updates the shortcut on your desktop that starts %s directly, without the launcher window. Desktop folder:", game(id).title)
                                          : trf("SafeHouse puts a shortcut on your desktop that starts %s directly, without the launcher window. Desktop folder:", game(id).title)).c_str(),
                        fonts().body, color::text2);
            caption(m_shortcutFolder.empty() ? tr("(not found)") : m_shortcutFolder.c_str(), color::text);
            if (st.regions.size() > 1) {
                gap(4);
                textWrapped(trf("The shortcut starts the region selected here, now %s. Pick another region in SafeHouse and the shortcut starts that one.",
                                tr(st.regionName()).c_str()).c_str(), fonts().body, color::text2);
            }
            gap(4);
#ifdef _WIN32
            toggleRow(tr("Also add it to the Start menu"), &m_shortcutMenu, nullptr, px(48));
#else
            toggleRow(tr("Also add it to the applications menu"), &m_shortcutMenu, nullptr, px(48));
#endif
            caption(shortcut::iconCredit(id));
            gap(8);
            const float w = std::floor((ImGui::GetContentRegionAvail().x - px(12)) * 0.5f);
            if (button(tr("Cancel"), Tone::Secondary, { w, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            takeFocusNow();
            ImGui::SameLine(0, px(12));
            if (button(m_shortcutExists ? tr("Update shortcut") : tr("Add shortcut"), Tone::Primary, { w, 0 }, m_shortcutExists ? Icon::Refresh : Icon::Plus)) {
                ImGui::CloseCurrentPopup();
                if (!m_fixture) createShortcut(id);
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        if (m_showShortcutResult) {
            ImGui::OpenPopup("Shortcut##result");
            m_showShortcutResult = false;
        }
        ImGui::SetNextWindowSize({ px(540), 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(24), px(22) });
        if (ImGui::BeginPopupModal("Shortcut##result", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            heading(m_shortcutHeading.c_str());
            textWrapped(m_shortcutMessage.c_str(), fonts().body, m_shortcutOk ? color::text2 : color::warning);
            gap(8);
            if (button(tr("Close"), Tone::Secondary, { 0, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            takeFocusNow();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    void drawDialogs() {
        drawSly3Notice();
        drawSteamDialogs();
        drawShortcutDialogs();
        drawReextractDialog();
        if (m_showPreviewError) {
            ImGui::OpenPopup("Renderer setting##error");
            m_showPreviewError = false;
        }
        ImGui::SetNextWindowSize({px(480), 0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {px(24), px(22)});
        if (ImGui::BeginPopupModal("Renderer setting##error", nullptr,
                                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            heading(tr("Could not save renderer setting"));
            textWrapped(m_previewError.c_str(), fonts().body, color::text2);
            gap(8);
            if (button(tr("Close"), Tone::Secondary, {0, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            takeFocusNow();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        if (m_confirmUninstall >= 0) {
            ImGui::OpenPopup("Uninstall##confirm");
            m_pending = m_confirmUninstall;
            m_confirmUninstall = -1;
        }
        ImGui::SetNextWindowSize({ px(480), 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(24), px(22) });
        if (ImGui::BeginPopupModal("Uninstall##confirm", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            const GameId id = static_cast<GameId>(std::clamp(m_pending, 0, kGameCount - 1));
            const auto& st = m_status[static_cast<size_t>(id)];
            const std::string which = st.regions.size() > 1 ? std::string(game(id).shortName) + " (" + tr(st.regionName()) + ")" : std::string(game(id).shortName);
            heading(trf("Uninstall %s?", which.c_str()).c_str());
            textWrapped(((st.regions.size() > 1 ? trf("The installed game files of the %s version are removed; your other regions stay.", tr(st.regionName()).c_str())
                                                : std::string(tr("The installed game files are removed."))) + " " +
                         tr("Your saves and settings are kept, and you can install again from your disc image at any time.")).c_str(),
                        fonts().body, color::text2);
            gap(8);
            const float w = std::floor((ImGui::GetContentRegionAvail().x - px(12)) * 0.5f);
            if (button(tr("Cancel"), Tone::Secondary, { w, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            takeFocusNow();
            ImGui::SameLine(0, px(12));
            if (button(tr("Uninstall"), Tone::Danger, { w, 0 }, Icon::Trash)) {
                ImGui::CloseCurrentPopup();
                if (!m_fixture)
                    m_job.start("Uninstall", [this, id](const auto& log) {
                        m_backend.uninstall(id);
                        log(std::string(game(id).title) + " uninstalled");
                    });
                invalidate();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

private:
    static uint32_t mix(uint32_t a, uint32_t b, float t) {
        auto channel = [&](int shift) {
            const float x = static_cast<float>((a >> shift) & 255), y = static_cast<float>((b >> shift) & 255);
            return static_cast<uint32_t>(std::lround(x + (y - x) * t)) << shift;
        };
        return channel(16) | channel(8) | channel(0);
    }
    // Text wrapped at the wrap position pushed by the caller (textWrapped()
    // would wrap at the window edge, under the action buttons).
    static void wrapped(const char* value, ImFont* font, uint32_t rgb) {
        FontScope f(font);
        ColorScope c(rgb);
        ImGui::TextUnformatted(value);
    }
    // "Add to Steam" for a game that can be played.
#if defined(CANE_IOS)
    // (iOS: no Steam, no desktop, and no home-screen shortcuts an app can add: the row is left out)
    static bool steamRow(const GameStatus&) { return false; }
#else
    static bool steamRow(const GameStatus& s) { return s.assets && s.runtime; }
#endif
    // "Add desktop shortcut" (Sly 1 and Sly 2 only for now), on the Steam row.
    static bool shortcutRow(GameId id, const GameStatus& s) { return steamRow(s) && shortcut::supported(id); }
    static const char* steamLabel(const GameStatus& s) { return s.inSteam ? N_("Update Steam shortcut") : N_("Add to Steam"); }
    static const char* shortcutLabel(const GameStatus& s) { return s.onDesktop ? N_("Update desktop shortcut") : N_("Add desktop shortcut"); }
    static float quietWidth(const char* label) { return measure(fonts().body, label).x + px(18 + 10 + 12 * 2); }
    static bool shortcutBesideSteam(const GameStatus& s, float width) {
        return quietWidth(tr(steamLabel(s))) + px(4) + quietWidth(tr(shortcutLabel(s))) <= width;
    }
#ifdef CANE_ANDROID
    void pinShortcut(GameId id) {
        m_shortcutGame = static_cast<int>(id);
        m_shortcutHeading = tr("Add shortcut");
        try {
            const std::string icon = shortcut::iconImage(id);  // (none yet for Sly 3: Android shows the app's icon)
            m_shortcutOk = android::pinShortcut(std::string("play-") + game(id).key, game(id).shortName, icon, { "--play", game(id).key });
            m_shortcutMessage = m_shortcutOk ? trf("Confirm the shortcut in the window that opens. It starts %s directly, without the launcher, "
                                                   "and closes the app when you quit the game.", game(id).title)
                                             : std::string(tr("This device's home screen does not take shortcuts from apps."));
        } catch (const std::exception& e) {
            m_shortcutOk = false;
            m_shortcutMessage = trf("Could not add the shortcut: %s.", tr(std::string(e.what())).c_str());
        }
        if (!m_shortcutOk) m_showShortcutResult = true;  // (on success Android shows its own confirmation)
    }
#endif
    void createShortcut(GameId id) {
        try {
            const auto desktop = shortcut::create(id, shortcut::Where::Desktop);
            std::string text = (desktop.existed ? trf("The desktop shortcut for %s is up to date:", game(id).title) : trf("%s has a shortcut on your desktop now:", game(id).title)) +
                               "\n" + shortcut::displayPath(desktop.file);
            if (m_shortcutMenu) {
                const auto menu = shortcut::create(id, shortcut::Where::Menu);
                text += "\n\n" + trf("and in the %s:", tr(shortcut::whereName(shortcut::Where::Menu))) + "\n" + shortcut::displayPath(menu.file);
            }
#ifndef _WIN32
            if (!desktop.trusted)
                text += std::string("\n\n") + tr("If your desktop shows it as a file, right-click it and choose \"Allow Launching\".");
#endif
            m_shortcutHeading = desktop.existed ? tr("Desktop shortcut updated") : tr("Desktop shortcut added");
            m_shortcutMessage = text;
            m_shortcutOk = true;
        } catch (const std::exception& e) {
            m_shortcutHeading = tr("Desktop shortcut not added");
            m_shortcutMessage = trf("Could not create the shortcut: %s", tr(std::string(e.what())).c_str());
            m_shortcutOk = false;
        }
        m_showShortcutResult = true;
        invalidate();
    }
    static int secondaryCount(GameId id, const GameStatus& s, bool) { return installable(id) && (s.assets || s.repair) ? 1 : 0; }
    // The 2-column button grid under Play: "Show progress" while installing; else Verify files / Reinstall /
    // Add region / Uninstall (+ Export textures on a Sly 2 install from before 2026-11-04) once installed.
    static int gridRows(const GameStatus& s, bool installing) {
        if (installing) return 1;
        return s.assets ? (4 + (s.texturesMissing ? 1 : 0) + 1) / 2 : 0;
    }
    // The grid and, when not installing, the folder links under it.
    static float gridHeight(const GameStatus& s, bool installing) {
        const int rows = gridRows(s, installing);
        return px(12) + px(38) * rows + px(8) * std::max(rows - 1, 0) + (installing ? 0.0f : px(8) + px(30));
    }
    void takeFocusNow() {
        if (ImGui::IsWindowAppearing()) setFocus(GImGui->LastItemData.ID, ImGui::GetCurrentWindow(), !input::tracker().lastWasMouse());
    }
    std::string detail(GameId id, const GameStatus& s, bool installing) const {
        const GameInfo& g = game(id);
        if (!installable(id)) return trf("A native port of %s is in development. There is nothing to install yet.", g.title);
        if (installing) return tr("Installing your game files. Keep the launcher open until it finishes.");
        if (s.outdated)
            return trf("A SafeHouse update changed the format of the %s game files, so they must be extracted from your disc "
                       "again before you can play. It takes a few minutes; your saves and settings are kept.", g.shortName);
        if (s.repair) return trf("This installation is damaged or incomplete. Repair it from your %s disc image.", g.shortName);
        if (s.assets) {
            std::string from = s.iso.empty() ? std::string(tr("Game files installed")) : trf("Installed from %s", utf8(path(s.iso).filename()).c_str());
            if (!s.runtime) from += ". " + trf("The %s port is not ready to play yet; your files are ready for it.", g.shortName);
            return from;
        }
        std::string text = trf("Install from your own %s disc image (%s). Needs %s of free space.", g.shortName, supportedRegionList(id).c_str(),
                               spaceRange(id).c_str());
        if (!s.runtime) text += std::string(" ") + tr("The port itself is still in development.");
        return text;
    }
    // The region dropdown (shown with two or more installed regions). The choice is remembered at once
    // (launcher.json "regions"): Play, the folders, Verify and Add to Steam then use that region.
    void regionPicker(GameId id, const GameStatus& s, float width, bool busy) {
        const float labelW = px(84);
        const ImVec2 p = ImGui::GetCursorPos();
        ImGui::SetCursorPosY(p.y + (controlHeight() - fonts().caption->FontSize) * 0.5f);
        text(tr("Region"), fonts().tab, color::text2);
        ImGui::SetCursorPos({ p.x + labelW, p.y });
        ImGui::BeginDisabled(busy);
        pushComboStyle();
        ImGui::SetNextItemWidth(width - labelW);
        const std::string preview = tr(s.regionName()) + (s.outdated ? std::string("  ·  ") + tr("update needed") : std::string());
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (beginCombo("##region", preview.c_str())) {
            for (const auto& r : s.regions) {
                const std::string label = tr(r.name()) + (r.outdated ? std::string("  ·  ") + tr("update needed") : !r.current ? std::string("  ·  ") + tr("needs repair") : std::string());
                if (ImGui::Selectable(label.c_str(), r.region == s.region) && r.region != s.region && !m_fixture) {
                    try {
                        m_backend.setSelectedRegion(id, r.region);
                    } catch (...) {
                    }
                    invalidate();
                }
                if (r.region == s.region && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        recordLayout("region", ImRect(at, { at.x + width - labelW, at.y + controlHeight() }));
        popComboStyle();
        ImGui::EndDisabled();
        ImGui::SetCursorPos({ p.x, p.y + controlHeight() + px(10) });
    }
    // "Re-extract": the asset format changed. With the remembered disc image: extract it again now, or choose
    // another; without it: choose the disc image on the install page.
    void drawReextractDialog() {
        if (m_confirmReextract >= 0) {
            ImGui::OpenPopup("Re-extract##confirm");
            m_reextractGame = m_confirmReextract;
            m_confirmReextract = -1;
            const GameId id = static_cast<GameId>(m_reextractGame);
            const auto& st = m_status[static_cast<size_t>(id)];
            m_reextractIso = st.iso;
            if (m_reextractIso.empty() && !m_fixture) m_reextractIso = m_backend.rememberedIso(id);
            std::error_code ec;
            m_reextractIsoFound = !m_reextractIso.empty() && (m_fixture || fs::is_regular_file(path(m_reextractIso), ec));
        }
        ImGui::SetNextWindowSize({ px(560), 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(24), px(22) });
        if (ImGui::BeginPopupModal("Re-extract##confirm", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            const GameId id = static_cast<GameId>(std::clamp(m_reextractGame, 0, kGameCount - 1));
            const auto& st = m_status[static_cast<size_t>(id)];
            const std::string region = st.regionName();
            heading(trf("Re-extract %s?", (std::string(game(id).shortName) + (region.empty() ? "" : " (" + tr(region) + ")")).c_str()).c_str());
            textWrapped(trf("A SafeHouse update changed the format of the %s game files. The files installed from your disc are in the old format, so the disc must be extracted again "
                            "before you can play. This takes a few minutes. Your saves and settings are not touched.", game(id).shortName).c_str(),
                        fonts().body, color::text2);
            gap(6);
            if (m_reextractIsoFound) {
                text(tr("Disc image"), fonts().caption, color::muted);
                tighten(4);
                textWrapped(utf8(path(m_reextractIso).filename()).c_str(), fonts().tab, color::text);
                tighten(4);
                textWrapped(utf8(path(m_reextractIso).parent_path()).c_str(), fonts().caption, color::muted);
            } else if (!m_reextractIso.empty())
                textWrapped(trf("The disc image it was installed from was not found: %s. Choose your disc image.", m_reextractIso.c_str()).c_str(),
                            fonts().body, color::warning);
            gap(8);
            const float w = std::floor((ImGui::GetContentRegionAvail().x - px(12) * 2) / 3.0f);
            if (button(tr("Cancel"), Tone::Secondary, { w, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            ImGui::SameLine(0, px(12));
            if (button(m_reextractIsoFound ? tr("Other disc…") : tr("Choose disc…"), m_reextractIsoFound ? Tone::Secondary : Tone::Primary, { w, 0 },
                       Icon::Disc)) {
                ImGui::CloseCurrentPopup();
                if (!m_fixture && openInstall) openInstall(id);
            }
            if (!m_reextractIsoFound) takeFocusNow();
            if (m_reextractIsoFound) {
                ImGui::SameLine(0, px(12));
                if (button(tr("Re-extract"), Tone::Cta, { w, 0 }, Icon::Refresh)) {
                    ImGui::CloseCurrentPopup();
                    if (!m_fixture && reextract) {
                        installingGame = static_cast<int>(id);
                        reextract(id, m_reextractIso);
                    }
                }
                takeFocusNow();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    // Sly 3's warning banner (brand::sly3Notice) at the top of its page; returns its height.
    float earlyBanner(ImVec2 p, float width) {
        auto* dl = ImGui::GetWindowDrawList();
        ImFont* f = fonts().body;
        const float iw = px(20), padX = px(16), textW = width - padX * 2 - iw - px(12);
        const auto t = f->CalcTextSizeA(f->FontSize, FLT_MAX, textW, tr(brand::sly3Notice));
        const float h = std::max(px(44), t.y + px(20));
        const ImVec2 b{ p.x + width, p.y + h };
        dl->AddRectFilled(p, b, ink(color::warning, 0.12f), px(10));
        dl->AddRect(p, b, ink(color::warning, 0.55f), px(10), 0, px(1));
        drawIcon(dl, Icon::Warning, { p.x + padX + iw * 0.5f, p.y + h * 0.5f }, iw, ink(color::warning));
        dl->AddText(f, f->FontSize, { p.x + padX + iw + px(12), std::round(p.y + (h - t.y) * 0.5f) }, ink(color::text), tr(brand::sly3Notice),
                    nullptr, textW);
        recordLayout("sly3-notice", ImRect(p, b));
        return h;
    }
    // The one-time confirmation before Sly 3 starts ("Don't show again": launcher.json "sly3_notice_hidden").
    void drawSly3Notice() {
        if (m_confirmSly3) {
            ImGui::OpenPopup("Sly 3##notice");
            m_confirmSly3 = false;
            m_sly3DontShow = false;
        }
        ImGui::SetNextWindowSize({ px(540), 0 });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(24), px(22) });
        if (ImGui::BeginPopupModal("Sly 3##notice", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            heading(tr("Sly 3 is in early development"));
            textWrapped(tr("Sly 3: Honor Among Thieves is not fully playable yet. Expect missing features, visual and audio problems, "
                        "and crashes. Progress may stop at points the port cannot handle yet."),
                        fonts().body, color::text2);
            gap(4);
            toggleRow(tr("Don't show again"), &m_sly3DontShow, nullptr, px(48));
            gap(8);
            const float w = std::floor((ImGui::GetContentRegionAvail().x - px(12)) * 0.5f);
            if (button(tr("Cancel"), Tone::Secondary, { w, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            ImGui::SameLine(0, px(12));
            if (button(tr("Play anyway"), Tone::Cta, { w, 0 }, Icon::Play)) {
                ImGui::CloseCurrentPopup();
                if (m_sly3DontShow && !m_fixture) try {
                        m_backend.hideSly3Notice();
                    } catch (...) {
                    }
                m_sly3Confirmed = true;
                play(GameId::Sly3);
            }
            takeFocusNow();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    // The call-to-action button (Play / Install), larger than the others.
    static bool bigButton(const char* label, ImVec2 size, Icon icon) {
        return button(label, Tone::Cta, size, icon, fonts().heading);
    }
    // A non-interactive plate in place of the main action.
    static void badge(const char* label, ImVec2 size) {
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, { p.x + size.x, p.y + size.y }, ink(color::surface, 0.85f), px(8));
        dl->AddRect(p, { p.x + size.x, p.y + size.y }, ink(color::borderStrong), px(8), 0, px(1));
        const auto t = measure(fonts().heading, label);
        const float iw = px(22), x = std::round(p.x + (size.x - t.x - iw - px(10)) * 0.5f);
        drawIcon(dl, Icon::Wrench, { x + iw * 0.5f, p.y + size.y * 0.5f }, iw, ink(color::text2));
        dl->AddText(fonts().heading, fonts().heading->FontSize, { x + iw + px(10), std::round(p.y + (size.y - t.y) * 0.5f) }, ink(color::text2), label);
        recordLayout(label, ImRect(p, { p.x + size.x, p.y + size.y }));
        ImGui::Dummy(size);
    }
    void progressButton(ImVec2 size) {
        float progress = 0;
        std::string stage;
        {
            std::lock_guard lock(m_job.mutex);
            progress = m_job.progress;
            stage = m_job.stage;
        }
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, { p.x + size.x, p.y + size.y }, ink(color::surface), px(8));
        dl->AddRectFilled(p, { p.x + size.x * std::clamp(progress, 0.0f, 1.0f), p.y + size.y }, ink(color::primary, 0.55f), px(8));
        dl->AddRect(p, { p.x + size.x, p.y + size.y }, ink(color::borderStrong), px(8), 0, px(1));
        char label[96];
        std::snprintf(label, sizeof(label), "%s · %.0f%%", stage.empty() ? tr("Installing") : stageLabel(stage).c_str(), progress * 100);
        const auto t = measure(fonts().tab, label);
        dl->AddText(fonts().tab, fonts().tab->FontSize, { std::round(p.x + (size.x - t.x) * 0.5f), std::round(p.y + (size.y - t.y) * 0.5f) },
                    ink(color::text), label);
        ImGui::Dummy(size);
    }
    void play(GameId id) {
        if (m_job.busy) return;
        // Sly 3 asks first (until "Don't show again").
        if (id == GameId::Sly3 && !m_sly3Confirmed && !m_backend.sly3NoticeHidden()) {
            m_confirmSly3 = true;
            return;
        }
        m_sly3Confirmed = false;
        if (m_fixture) return;
        playingGame = static_cast<int>(id);
        m_job.start(std::string("Playing ") + game(id).shortName, [this, id](const auto& log) { m_backend.play(id, m_job.cancel, log); });
    }
    void verify(GameId id) {
        if (m_fixture || m_job.busy) return;
        m_job.start("Verification", [this, id](const auto& log) {
            verifyInstallation(
                m_backend.dataDir(id),
                [&](size_t n, size_t total, const std::string& name) {
                    log(json({ { "stage", "verify" }, { "completed", n }, { "total", total }, { "item", name } }).dump());
                },
                [&] { return m_job.cancel.load(); });
        });
    }
    void exportTextures(GameId id) {
        if (m_fixture || m_job.busy) return;
        m_job.start("Texture export", [this, id](const auto& log) { m_backend.exportTextures(id, m_job.cancel, log); });
    }
    void openFolderSafe(const fs::path& folder) {
        if (m_fixture) return;
        try {
            fs::create_directories(folder);
            openFolder(folder);
        } catch (const std::exception& e) {
            std::lock_guard lock(m_job.mutex);
            m_job.message = e.what();
        }
    }
    void refresh() {
        if (m_fixture) return;
        const double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
        const bool busy = m_job.busy.load();
        if (!((m_wasBusy && !busy) || now >= m_nextCheck)) {
            m_wasBusy = busy;
            return;
        }
        m_wasBusy = busy;
        m_nextCheck = now + 1.0;
        try {
            m_steam = steam::find();
        } catch (...) {
            m_steam.reset();
        }
        for (const auto& g : games()) {
            auto& s = m_status[static_cast<size_t>(g.id)];
            s.regions = m_backend.installations(g.id);
            s.region = m_backend.selectedRegion(g.id);
            s.assets = installable(g.id) && m_backend.assetsInstalled(g.id);
            s.runtime = m_backend.runtimeAvailable(g.id);
            s.repair = installable(g.id) && !s.assets && m_backend.needsRepair(g.id);
            s.outdated = installable(g.id) && m_backend.needsReextract(g.id);
            s.texturesMissing = m_backend.texturesMissing(g.id);
            s.iso = s.assets || s.outdated ? m_backend.rememberedIso(g.id) : std::string{};
            s.inSteam = m_steam && s.assets && s.runtime && steam::added(*m_steam, g.id);
            s.onDesktop = s.assets && s.runtime && shortcut::supported(g.id) && shortcut::exists(g.id, shortcut::Where::Desktop);
        }
    }
    LauncherBackend& m_backend;
    Job& m_job;
    std::array<GameStatus, kGameCount> m_status{};
    double m_nextCheck = 0;
    bool m_wasBusy = false, m_fixture = false;
    bool m_showPreviewError = false;
    std::string m_previewError;
    int m_confirmUninstall = -1, m_pending = -1;
    bool m_confirmSly3 = false, m_sly3Confirmed = false, m_sly3DontShow = false;
    std::optional<steam::Installation> m_steam;
    int m_confirmSteam = -1, m_steamGame = -1;
    int m_confirmReextract = -1, m_reextractGame = -1;
    std::string m_reextractIso;
    bool m_reextractIsoFound = false;
    bool m_steamRunning = false, m_showSteamResult = false, m_steamOk = false;
    std::string m_steamMessage;
    int m_confirmShortcut = -1, m_shortcutGame = -1;
    bool m_shortcutExists = false, m_shortcutMenu = false, m_showShortcutResult = false, m_shortcutOk = false;
    std::string m_shortcutFolder, m_shortcutHeading, m_shortcutMessage;
};
} // namespace cane::ui
