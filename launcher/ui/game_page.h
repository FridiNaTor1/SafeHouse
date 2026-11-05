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
        m_shortcutHeading = "Desktop shortcut added";
        try {
            m_shortcutMessage = std::string(cane::game(game).title) + " has a shortcut on your desktop now:\n" +
                                shortcut::displayPath(shortcut::file(game, shortcut::Where::Desktop)) + "\n\nand in the " +
                                shortcut::whereName(shortcut::Where::Menu) + ":\n" + shortcut::displayPath(shortcut::file(game, shortcut::Where::Menu));
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
        std::vector<launcher::Hint> h{ { Button::LB, 0, "" }, { Button::RB, 0, "Games" } };
        const auto& s = status(game);
        if (!m_job.busy && s.assets && s.runtime) h.push_back({ Button::Start, 0, "Play" });
        else if (!m_job.busy && installable(game))
            h.push_back({ Button::Start, 0, s.outdated ? "Re-extract" : s.repair ? "Repair" : s.assets ? "Reinstall" : "Install" });
        h.push_back({ backButton(), GLFW_KEY_ESCAPE, "Back" });
        h.push_back({ confirmButton(), GLFW_KEY_ENTER, "Select" });
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
        constexpr const char* previewHelp = "Experimental. GS compatibility stays active; F8 switches views.";
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
            if (playing) pill = "Playing", tone = color::success;
            else if (installing) pill = "Installing", tone = color::accent;
            else if (!installable(id)) pill = "In development", tone = color::accent;
            else if (s.outdated) pill = "Update needed", tone = color::warning;
            else if (s.repair) pill = "Needs repair", tone = color::warning;
            else if (s.assets && s.runtime) pill = "Ready to play", tone = color::success;
            else if (s.assets) pill = "Installed", tone = color::success;
            else pill = "Not installed", tone = color::muted;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawPill(dl, p, pill.c_str(), tone);
            float x = p.x + pillSize(pill.c_str()).x + px(8);
            if (s.assets && !s.runtime && installable(id)) {
                const char* dev = "Port in development";
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
            wrapped(g.blurb, fonts().body, color::text2);
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
            if (button("Stop game", Tone::Danger, cta, Icon::Stop)) m_job.cancel = true;
            takeFocus(!input::tracker().lastWasMouse());
        } else if (installing) {
            progressButton(cta);
        } else if (s.assets && !s.runtime) {
            badge("In development", cta);
        } else {
            ImGui::BeginDisabled(busy);
            const bool ready = s.assets && s.runtime;
            const char* label = ready ? "Play" : s.outdated ? "Re-extract" : s.repair ? "Repair" : "Install";
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
            if (toggleRow("Native renderer preview", &enabled, nullptr, px(48)) && !m_fixture) {
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
                if (button("Show progress", Tone::Secondary, { half, 0 }, Icon::ChevronRight) && openInstall) openInstall(id);
                takeFocus(!input::tracker().lastWasMouse());
                ImGui::BeginDisabled(busy);
            }
            if (s.assets && !installing) {
                next();
                if (button("Verify files", Tone::Secondary, { half, 0 }, Icon::Check)) verify(id);
                takeFocus(!input::tracker().lastWasMouse());
                next();
                if (button("Reinstall", Tone::Secondary, { half, 0 }, Icon::Disc) && openInstall) openInstall(id);
                // Another region / release beside this one (the Region menu above Play then switches between them).
                next();
                if (button("Add region", Tone::Secondary, { half, 0 }, Icon::Plus) && addRegion) addRegion(id);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("Install another region of %s (%s) beside this one.\n"
                                      "Your installed regions stay; pick one with the Region menu above Play.\n"
                                      "Saves and settings are shared by the regions.", game(id).shortName, supportedRegionList(id).c_str());
                next();
                if (button("Uninstall", Tone::Secondary, { half, 0 }, Icon::Trash)) m_confirmUninstall = static_cast<int>(id);
                // (Sly 2 installed before 2026-11-04: its texture dump, a third row of the grid; gridRows)
                if (s.texturesMissing) {
                    next();
                    if (button("Export textures", Tone::Secondary, { half, 0 }, Icon::Folder)) exportTextures(id);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                        ImGui::SetTooltip("Write every texture of the game as a PNG into the game folder's Textures folder,\n"
                                          "for texture mods (about 80 MB, under a minute). Newer installs have it already.");
                }
            }
            ImGui::EndDisabled();
            if (!installing) {
                gap(0);
                if (button("Open save folder", Tone::Quiet, { 0, px(30) }, Icon::Folder)) openFolderSafe(m_backend.userDir(id));
                if (s.assets || s.repair) {
                    ImGui::SameLine(0, px(4));
                    if (button("Open game folder", Tone::Quiet, { 0, px(30) }, Icon::Folder)) openFolderSafe(m_backend.dataDir(id));
                }
            }
        }
        if (steamRow(s) && !installing) {
            gap(0);
            // Disabled without Steam; the tooltip says why (also over the disabled button).
            ImGui::BeginDisabled(busy || !m_steam);
            if (button(s.inSteam ? "Update Steam shortcut" : "Add to Steam", Tone::Quiet, { 0, px(30) }, s.inSteam ? Icon::Refresh : Icon::Plus))
                m_confirmSteam = static_cast<int>(id);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("%s", !m_steam ? "Steam was not found on this computer"
                                       : s.inSteam ? "The shortcut is in Steam. Update it (for example after moving SafeHouse)"
                                                   : "Add a Steam shortcut that starts this game directly, with its artwork");
            // "Add desktop shortcut" (Sly 1, Sly 2): beside "Add to Steam" when both fit, else under it.
            if (shortcutRow(id, s)) {
                if (shortcutBesideSteam(s, actionsW)) ImGui::SameLine(0, px(4));
                ImGui::BeginDisabled(busy);
                if (button(shortcutLabel(s), Tone::Quiet, { 0, px(30) }, s.onDesktop ? Icon::Refresh : Icon::Plus))
                    m_confirmShortcut = static_cast<int>(id);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("%s", s.onDesktop ? "The shortcut is on your desktop. Update it (for example after moving SafeHouse)"
                                                        : "Put a shortcut that starts this game directly on your desktop");
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
            heading(("Add " + std::string(game(id).shortName) + " to Steam?").c_str());
            const int accounts = m_steam ? static_cast<int>(m_steam->accounts.size()) : 0;
            textWrapped(("SafeHouse adds a shortcut that starts " + std::string(game(id).title) + " directly, with its library artwork, to "
                         + (accounts == 1 ? std::string("your Steam account") : std::to_string(accounts) + " Steam accounts")
                         + ". Steam's shortcut list is backed up first.").c_str(), fonts().body, color::text2);
            if (m_status[static_cast<size_t>(id)].regions.size() > 1) {
                gap(4);
                textWrapped(("The shortcut starts the region selected here, now " + m_status[static_cast<size_t>(id)].regionName() +
                             ". Pick another region in SafeHouse and Steam starts that one.").c_str(), fonts().body, color::text2);
            }
            if (m_steamRunning) {
                gap(4);
                textWrapped("Steam is running. It only reads shortcuts when it starts, and it can write its own list back over the new "
                            "shortcut. Exit Steam first for the safest result, or restart Steam after adding.",
                            fonts().body, color::warning);
            }
            gap(8);
            const float w = std::floor((ImGui::GetContentRegionAvail().x - px(12)) * 0.5f);
            if (button("Cancel", Tone::Secondary, { w, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            takeFocusNow();
            ImGui::SameLine(0, px(12));
            if (button("Add to Steam", Tone::Primary, { w, 0 }, Icon::Plus)) {
                ImGui::CloseCurrentPopup();
                if (!m_fixture && m_steam) {
                    try {
                        const auto r = steam::add(*m_steam, id);
                        m_steamMessage = std::string(game(id).title) + (r.addedEntries ? " was added to " : " is up to date in ")
                                         + (r.accounts == 1 ? std::string("your Steam library") : std::to_string(r.accounts) + " Steam libraries")
                                         + (steam::running(*m_steam) ? ". Restart Steam to see it." : ". It shows the next time Steam starts.");
                        m_steamOk = true;
                    } catch (const std::exception& e) {
                        m_steamMessage = std::string("Could not add the shortcut: ") + e.what() + ". Steam's files were not changed.";
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
            heading(m_steamOk ? "Added to Steam" : "Steam shortcut not added");
            textWrapped(m_steamMessage.c_str(), fonts().body, color::text2);
            gap(8);
            if (button("Close", Tone::Secondary, { 0, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
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
            heading(((m_shortcutExists ? "Update the desktop shortcut for " : "Add a desktop shortcut for ") + std::string(game(id).shortName) + "?").c_str());
            textWrapped((std::string(m_shortcutExists ? "SafeHouse updates the shortcut on your desktop" : "SafeHouse puts a shortcut on your desktop") +
                         " that starts " + game(id).title + " directly, without the launcher window. Desktop folder:").c_str(),
                        fonts().body, color::text2);
            caption(m_shortcutFolder.empty() ? "(not found)" : m_shortcutFolder.c_str(), color::text);
            if (st.regions.size() > 1) {
                gap(4);
                textWrapped(("The shortcut starts the region selected here, now " + st.regionName() +
                             ". Pick another region in SafeHouse and the shortcut starts that one.").c_str(), fonts().body, color::text2);
            }
            gap(4);
#ifdef _WIN32
            toggleRow("Also add it to the Start menu", &m_shortcutMenu, nullptr, px(48));
#else
            toggleRow("Also add it to the applications menu", &m_shortcutMenu, nullptr, px(48));
#endif
            caption(shortcut::iconCredit(id));
            gap(8);
            const float w = std::floor((ImGui::GetContentRegionAvail().x - px(12)) * 0.5f);
            if (button("Cancel", Tone::Secondary, { w, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            takeFocusNow();
            ImGui::SameLine(0, px(12));
            if (button(m_shortcutExists ? "Update shortcut" : "Add shortcut", Tone::Primary, { w, 0 }, m_shortcutExists ? Icon::Refresh : Icon::Plus)) {
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
            if (button("Close", Tone::Secondary, { 0, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
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
            heading("Could not save renderer setting");
            textWrapped(m_previewError.c_str(), fonts().body, color::text2);
            gap(8);
            if (button("Close", Tone::Secondary, {0, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
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
            const std::string which = st.regions.size() > 1 ? " (" + st.regionName() + ")" : std::string{};
            heading(("Uninstall " + std::string(game(id).shortName) + which + "?").c_str());
            textWrapped(((st.regions.size() > 1 ? "The installed game files of the " + st.regionName() + " version are removed; your other regions stay. "
                                                : std::string("The installed game files are removed. ")) +
                         "Your saves and settings are kept, and you can install again from your disc image at any time.").c_str(),
                        fonts().body, color::text2);
            gap(8);
            const float w = std::floor((ImGui::GetContentRegionAvail().x - px(12)) * 0.5f);
            if (button("Cancel", Tone::Secondary, { w, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            takeFocusNow();
            ImGui::SameLine(0, px(12));
            if (button("Uninstall", Tone::Danger, { w, 0 }, Icon::Trash)) {
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
    static bool steamRow(const GameStatus& s) { return s.assets && s.runtime; }
    // "Add desktop shortcut" (Sly 1 and Sly 2 only for now), on the Steam row.
    static bool shortcutRow(GameId id, const GameStatus& s) { return steamRow(s) && shortcut::supported(id); }
    static const char* steamLabel(const GameStatus& s) { return s.inSteam ? "Update Steam shortcut" : "Add to Steam"; }
    static const char* shortcutLabel(const GameStatus& s) { return s.onDesktop ? "Update desktop shortcut" : "Add desktop shortcut"; }
    static float quietWidth(const char* label) { return measure(fonts().body, label).x + px(18 + 10 + 12 * 2); }
    static bool shortcutBesideSteam(const GameStatus& s, float width) {
        return quietWidth(steamLabel(s)) + px(4) + quietWidth(shortcutLabel(s)) <= width;
    }
    void createShortcut(GameId id) {
        try {
            const auto desktop = shortcut::create(id, shortcut::Where::Desktop);
            std::string text = std::string(game(id).title) + (desktop.existed ? "'s desktop shortcut is up to date:\n" : " has a shortcut on your desktop now:\n") +
                               shortcut::displayPath(desktop.file);
            if (m_shortcutMenu) {
                const auto menu = shortcut::create(id, shortcut::Where::Menu);
                text += std::string("\n\nand in the ") + shortcut::whereName(shortcut::Where::Menu) + ":\n" + shortcut::displayPath(menu.file);
            }
#ifndef _WIN32
            if (!desktop.trusted)
                text += "\n\nIf your desktop shows it as a file, right-click it and choose \"Allow Launching\".";
#endif
            m_shortcutHeading = desktop.existed ? "Desktop shortcut updated" : "Desktop shortcut added";
            m_shortcutMessage = text;
            m_shortcutOk = true;
        } catch (const std::exception& e) {
            m_shortcutHeading = "Desktop shortcut not added";
            m_shortcutMessage = std::string("Could not create the shortcut: ") + e.what();
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
        if (!installable(id)) return std::string("A native port of ") + g.title + " is in development. There is nothing to install yet.";
        if (installing) return "Installing your game files. Keep the launcher open until it finishes.";
        if (s.outdated)
            return std::string("A SafeHouse update changed the format of ") + g.shortName + "'s game files, so they must be extracted from your disc "
                   "again before you can play. It takes a few minutes; your saves and settings are kept.";
        if (s.repair) return std::string("This installation is damaged or incomplete. Repair it from your ") + g.shortName + " disc image.";
        if (s.assets) {
            std::string from = s.iso.empty() ? std::string("Game files installed") : "Installed from " + utf8(path(s.iso).filename());
            if (!s.runtime) from += ". The " + std::string(g.shortName) + " port is not ready to play yet; your files are ready for it.";
            return from;
        }
        std::string text = std::string("Install from your own ") + g.shortName + " disc image (" + supportedRegionList(id) + "). Needs " +
                           spaceRange(id) + " of free space.";
        if (!s.runtime) text += " The port itself is still in development.";
        return text;
    }
    // The region dropdown (shown with two or more installed regions). The choice is remembered at once
    // (launcher.json "regions"): Play, the folders, Verify and Add to Steam then use that region.
    void regionPicker(GameId id, const GameStatus& s, float width, bool busy) {
        const float labelW = px(84);
        const ImVec2 p = ImGui::GetCursorPos();
        ImGui::SetCursorPosY(p.y + (controlHeight() - fonts().caption->FontSize) * 0.5f);
        text("Region", fonts().tab, color::text2);
        ImGui::SetCursorPos({ p.x + labelW, p.y });
        ImGui::BeginDisabled(busy);
        pushComboStyle();
        ImGui::SetNextItemWidth(width - labelW);
        const std::string preview = s.regionName() + (s.outdated ? "  ·  update needed" : "");
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (beginCombo("##region", preview.c_str())) {
            for (const auto& r : s.regions) {
                const std::string label = r.name() + (r.outdated ? "  ·  update needed" : !r.current ? "  ·  needs repair" : "");
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
            heading(("Re-extract " + std::string(game(id).shortName) + (region.empty() ? "" : " (" + region + ")") + "?").c_str());
            textWrapped(("A SafeHouse update changed the format of " + std::string(game(id).shortName) +
                         "'s game files. The files installed from your disc are in the old format, so the disc must be extracted again "
                         "before you can play. This takes a few minutes. Your saves and settings are not touched.").c_str(),
                        fonts().body, color::text2);
            gap(6);
            if (m_reextractIsoFound) {
                text("Disc image", fonts().caption, color::muted);
                tighten(4);
                textWrapped(utf8(path(m_reextractIso).filename()).c_str(), fonts().tab, color::text);
                tighten(4);
                textWrapped(utf8(path(m_reextractIso).parent_path()).c_str(), fonts().caption, color::muted);
            } else if (!m_reextractIso.empty())
                textWrapped(("The disc image it was installed from was not found: " + m_reextractIso + ". Choose your disc image.").c_str(),
                            fonts().body, color::warning);
            gap(8);
            const float w = std::floor((ImGui::GetContentRegionAvail().x - px(12) * 2) / 3.0f);
            if (button("Cancel", Tone::Secondary, { w, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            ImGui::SameLine(0, px(12));
            if (button(m_reextractIsoFound ? "Other disc…" : "Choose disc…", m_reextractIsoFound ? Tone::Secondary : Tone::Primary, { w, 0 },
                       Icon::Disc)) {
                ImGui::CloseCurrentPopup();
                if (!m_fixture && openInstall) openInstall(id);
            }
            if (!m_reextractIsoFound) takeFocusNow();
            if (m_reextractIsoFound) {
                ImGui::SameLine(0, px(12));
                if (button("Re-extract", Tone::Cta, { w, 0 }, Icon::Refresh)) {
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
        const auto t = f->CalcTextSizeA(f->FontSize, FLT_MAX, textW, brand::sly3Notice);
        const float h = std::max(px(44), t.y + px(20));
        const ImVec2 b{ p.x + width, p.y + h };
        dl->AddRectFilled(p, b, ink(color::warning, 0.12f), px(10));
        dl->AddRect(p, b, ink(color::warning, 0.55f), px(10), 0, px(1));
        drawIcon(dl, Icon::Warning, { p.x + padX + iw * 0.5f, p.y + h * 0.5f }, iw, ink(color::warning));
        dl->AddText(f, f->FontSize, { p.x + padX + iw + px(12), std::round(p.y + (h - t.y) * 0.5f) }, ink(color::text), brand::sly3Notice,
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
            heading("Sly 3 is in early development");
            textWrapped("Sly 3: Honor Among Thieves is not fully playable yet. Expect missing features, visual and audio problems, "
                        "and crashes. Progress may stop at points the port cannot handle yet.",
                        fonts().body, color::text2);
            gap(4);
            toggleRow("Don't show again", &m_sly3DontShow, nullptr, px(48));
            gap(8);
            const float w = std::floor((ImGui::GetContentRegionAvail().x - px(12)) * 0.5f);
            if (button("Cancel", Tone::Secondary, { w, 0 }) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                ImGui::CloseCurrentPopup();
            ImGui::SameLine(0, px(12));
            if (button("Play anyway", Tone::Cta, { w, 0 }, Icon::Play)) {
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
        std::snprintf(label, sizeof(label), "%s · %.0f%%", stage.empty() ? "Installing" : stageLabel(stage).c_str(), progress * 100);
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
