#pragma once
// Launcher frame, laid out like the OpenGOAL launcher: a game rail on the
// left (one logo per game, then Settings, About and the community links), the selected page on the
// right, and a 44 px footer with controller/keyboard hints across the bottom.
// Also the activity-log drawer, toasts and the About page.
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include "brand.h"
#include "games.h"
#include "install.h"
#include "prompt_bar.h"
#include "widgets.h"

namespace cane::ui {
enum class View { Game, Install, Settings, About };
inline float footerHeight() { return px(44); }
// Opens a community link (brand.h) in the system browser; false when that failed.
inline bool openLink(const char* url) {
    try {
        openUrl(url);
        return true;
    } catch (...) {
        return false;
    }
}

// Right-stick scrolling: the innermost focused scroll region consumes it.
inline float& padScrollAmount() {
    static float amount = 0;
    return amount;
}
inline void padScrollHere() {
    float& amount = padScrollAmount();
    if (amount == 0 || !ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || ImGui::GetScrollMaxY() <= 0) return;
    ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() + amount * px(1100) * ImGui::GetIO().DeltaTime, 0.0f, ImGui::GetScrollMaxY()));
    amount = 0;
}

class Shell {
public:
    GameId game = GameId::Sly1;
    View view = View::Game;
    // Set when the game or view changed this frame (cleared by the caller).
    bool changed = true;
    void selectGame(GameId next) {
        if (next == game && view == View::Game) return;
        game = next;
        view = View::Game;
        changed = true;
    }
    void open(View next, bool focusPage = true) {
        if (next == view) return;
        view = next;
        changed = true;
        if (focusPage) requestFocus();
    }
    // LB/RB: previous/next game.
    void cycleGame(int direction) {
        selectGame(static_cast<GameId>((static_cast<int>(game) + direction + kGameCount) % kGameCount));
        m_focusRail = true;
    }
    // Puts the pad/keyboard focus on the rail item of the current page.
    void focusRail() { m_focusRail = true; }
    bool railFocused() const {
        const ImGuiID nav = GImGui->NavId;
        return nav && std::find(m_ids.begin(), m_ids.end(), nav) != m_ids.end();
    }
    static float railWidth(float windowWidth) { return std::clamp(std::round(windowWidth * 0.19f), px(196), px(248)); }
    void toast(std::string message, bool ok = true) { m_toasts.push_back({ std::move(message), ImGui::GetTime(), ok }); }

    // The rail, drawn straight into the launcher window so pad and keyboard
    // navigation move between it and the page.
    void rail(ImVec2 size) {
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetWindowPos();
        const float w = railWidth(size.x), bottom = o.y + size.y - footerHeight();
        dl->AddRectFilled(o, { o.x + w, bottom }, u32(color::sidebar));
        dl->AddLine({ o.x + w - px(1), o.y }, { o.x + w - px(1), bottom }, u32(color::divider), px(1));
        m_ids.clear();
        const float pad = px(14);
        // Brand: the emblem with "SafeHouse" beside it, a size smaller when the rail is narrow.
        const float logoH = px(60);
        ImFont* brandFont = brandWidth(logoH, fonts().title) <= w - pad * 2 ? fonts().title : fonts().heading;
        const float brandW = brandWidth(logoH, brandFont);
        drawBrand(dl, { std::round(o.x + (w - brandW) * 0.5f), o.y + px(16) }, logoH, brandFont);
        recordLayout("brand", ImRect({ std::round(o.x + (w - brandW) * 0.5f), o.y + px(16) },
                                     { std::round(o.x + (w + brandW) * 0.5f), o.y + px(16) + logoH }));
        float y = o.y + px(16) + logoH + px(16);
        dl->AddText(fonts().label, fonts().label->FontSize, { o.x + pad + px(6), y }, u32(color::muted), "GAMES");
        y += fonts().label->FontSize + px(8);
        // Footer rows first, so the game tiles can share the rest.
        const float rowH = px(44), linkH = px(30), rowsH = rowH * 2 + px(8) + px(12) + linkH + px(10);
        const float tileGap = px(6);
        const float tileH = std::clamp(std::floor((bottom - rowsH - px(12) - y - tileGap * (kGameCount - 1)) / kGameCount), px(48), px(86));
        for (const auto& g : games()) {
            item(g.key, { o.x + pad * 0.5f, y }, { o.x + w - pad * 0.5f, y + tileH }, view != View::Settings && view != View::About && game == g.id,
                 [&](ImDrawList* d, const ImRect& bb, bool selected, bool hovered) {
                     const Image& logo = images().games[static_cast<int>(g.id)];
                     if (!logo.texture) return;
                     const float maxW = bb.GetWidth() - px(36), maxH = bb.GetHeight() - px(16);
                     float lw = maxW, lh = lw / logo.aspect;
                     if (lh > maxH) lh = maxH, lw = lh * logo.aspect;
                     const ImVec2 a{ std::round(bb.GetCenter().x - lw * 0.5f + px(2)), std::round(bb.GetCenter().y - lh * 0.5f) };
                     const float alpha = selected ? 1.0f : hovered ? 0.85f : 0.5f;
                     d->AddImage(logo.texture, a, { a.x + lw, a.y + lh }, { 0, 0 }, { 1, 1 }, u32(0xFFFFFF, alpha));
                 },
                 [&, id = g.id] { selectGame(id); }, g.title);
            y += tileH + tileGap;
        }
        float ry = bottom - rowsH + px(4);
        dl->AddLine({ o.x + pad, ry }, { o.x + w - pad, ry }, u32(color::divider), px(1));
        ry += px(8);
        const auto row = [&](const char* id, const char* label, Icon icon, View target) {
            item(id, { o.x + pad * 0.5f, ry }, { o.x + w - pad * 0.5f, ry + rowH }, view == target,
                 [&](ImDrawList* d, const ImRect& bb, bool selected, bool hovered) {
                     const uint32_t tone = selected || hovered ? color::text : color::text2;
                     drawIcon(d, icon, { bb.Min.x + px(26), bb.GetCenter().y }, px(20), u32(tone));
                     ImFont* f = selected ? fonts().tab : fonts().body;
                     d->AddText(f, f->FontSize, { bb.Min.x + px(48), std::round(bb.GetCenter().y - f->FontSize * 0.5f) }, u32(tone), label);
                 },
                 [&, target] { open(target, false); }, nullptr);
            ry += rowH + px(4);
        };
        row("rail-settings", "Settings", Icon::Cog, View::Settings);
        row("rail-about", "About", Icon::Info, View::About);
        // The community links, side by side under About.
        const float linkW = std::floor((w - pad - px(4)) * 0.5f);
        link("rail-discord", { o.x + pad * 0.5f, ry }, { o.x + pad * 0.5f + linkW, ry + linkH }, "Discord", Icon::Chat, brand::discordUrl,
             brand::discordLabel);
        const ImGuiID discord = m_ids.empty() ? 0 : m_ids.back();
        link("rail-patreon", { o.x + w - pad * 0.5f - linkW, ry }, { o.x + w - pad * 0.5f, ry + linkH }, "Patreon", Icon::Heart,
             brand::patreonUrl, brand::patreonLabel);
        m_focusRail = false;
        // Right (or confirm) on the rail enters the page (on the Discord link, right moves to Patreon).
        const bool inRail = railFocused();
        if (inRail && navRightPressed() && !input::tracker().lastWasMouse() && GImGui->NavId != discord) requestFocus();
        // Coming back from the page (left), land on the selected item rather
        // than whichever rail item is nearest.
        if (inRail && !m_wasInRail && GImGui->NavJustMovedToId) m_focusRail = true;
        m_wasInRail = inRail;
        // A nav move lands one frame after its key press.
        m_navKey = navKeyDown();
    }
    void footer(ImVec2 size, std::span<const launcher::Hint> hints, input::Family family, const std::string& activity, float progress) {
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos();
        const float h = footerHeight();
        const ImVec2 a{ origin.x, origin.y + size.y - h };
        dl->AddRectFilled(a, { origin.x + size.x, origin.y + size.y }, u32(color::surface));
        dl->AddLine(a, { origin.x + size.x, a.y }, u32(color::divider), px(1));
        const float cy = a.y + h * 0.5f;
        if (!activity.empty()) {
            float x = origin.x + px(20);
            if (progress >= 0) {
                spinner(dl, { x + px(8), cy }, px(7));
                x += px(26);
            }
            dl->AddText(fonts().caption, fonts().caption->FontSize, { x, std::round(cy - fonts().caption->FontSize * 0.5f) }, u32(color::text2),
                        activity.c_str());
        }
        ImGui::PushFont(fonts().caption);
        launcher::drawPromptHints(dl, { origin.x + size.x - px(24), cy }, px(24), hints, u32(color::text), u32(color::text2), family);
        ImGui::PopFont();
    }
    // Slide-up log drawer above the footer, over the page.
    void drawer(ImVec2 size, bool* open, const std::deque<std::string>& lines, bool follow) {
        if (!*open) return;
        const float x0 = railWidth(size.x), w = size.x - x0;
        const float h = std::round(size.y * 0.38f);
        const ImVec2 origin = ImGui::GetWindowPos();
        ImGui::SetCursorScreenPos({ origin.x + x0, origin.y + size.y - footerHeight() - h });
        ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(0x0A1222));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { px(32), px(16) });
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
        ImGui::BeginChild("activity-drawer", { w, h }, ImGuiChildFlags_AlwaysUseWindowPadding);
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        dl->AddLine(p, { p.x + w, p.y }, u32(color::borderStrong), px(1));
        heading("Activity log");
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(w - px(32) - px(36));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - px(6));
        if (iconButton("Close log", Icon::Close, px(36))) *open = false;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, vec(color::bg));
        ImGui::BeginChild("activity-lines", { 0, 0 }, ImGuiChildFlags_Border);
        {
            // Font and colour must be popped inside the child: PopFont()
            // acts on the current window's draw list.
            FontScope f(fonts().caption);
            ColorScope c(color::text2);
            if (lines.empty()) ImGui::TextUnformatted("Nothing yet. Installs, checks, source builds and game output are shown here.");
            for (const auto& line : lines) ImGui::TextUnformatted(line.c_str());
            if (follow) ImGui::SetScrollHereY(1);
            padScrollHere();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
    void toasts(ImVec2 size) {
        const double now = ImGui::GetTime();
        while (!m_toasts.empty() && now - m_toasts.front().shown > 4.0) m_toasts.pop_front();
        auto* dl = ImGui::GetForegroundDrawList();
        const ImVec2 origin = ImGui::GetMainViewport()->Pos;
        float y = origin.y + size.y - footerHeight() - px(20);
        for (auto it = m_toasts.rbegin(); it != m_toasts.rend(); ++it) {
            const float age = static_cast<float>(now - it->shown);
            const float alpha = std::clamp(std::min(age * 6.0f, (4.0f - age) * 3.0f), 0.0f, 1.0f);
            const float maxW = size.x * 0.5f;
            const auto t = fonts().body->CalcTextSizeA(fonts().body->FontSize, FLT_MAX, maxW, it->text.c_str());
            const ImVec2 b{ origin.x + size.x - px(32), y }, a{ b.x - t.x - px(56), b.y - t.y - px(28) };
            dl->AddRectFilled(a, b, u32(color::raised, alpha), px(10));
            dl->AddRect(a, b, u32(color::borderStrong, alpha), px(10), 0, px(1));
            drawIcon(dl, it->ok ? Icon::Check : Icon::Warning, { a.x + px(22), (a.y + b.y) * 0.5f }, px(18),
                     u32(it->ok ? color::success : color::warning, alpha));
            dl->AddText(fonts().body, fonts().body->FontSize, { a.x + px(40), std::round((a.y + b.y - t.y) * 0.5f) }, u32(color::text, alpha),
                        it->text.c_str(), nullptr, maxW);
            y = a.y - px(10);
        }
    }

private:
    // A small link at the bottom of the rail. Unlike the rail items, moving the focus onto it does nothing:
    // only A / Enter / a click opens it in the browser.
    void link(const char* id, ImVec2 min, ImVec2 max, const char* label, Icon icon, const char* url, const char* tooltip) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImGuiID gid = window->GetID(id);
        const ImRect bb(min, max);
        ImGui::SetCursorScreenPos(min);
        ImGui::ItemSize(bb.GetSize());
        if (!ImGui::ItemAdd(bb, gid)) return;
        m_ids.push_back(gid);
        bool hovered = false, held = false;
        if (ImGui::ButtonBehavior(bb, gid, &hovered, &held) && !openLink(url)) toast("Could not open your web browser", false);
        auto* dl = window->DrawList;
        if (hovered) dl->AddRectFilled(bb.Min, bb.Max, ink(color::surface), px(8));
        const uint32_t tone = hovered ? color::text : color::text2;
        ImFont* f = fonts().caption;
        const auto t = measure(f, label);
        const float iw = px(16), x = std::round(bb.GetCenter().x - (iw + px(6) + t.x) * 0.5f);
        drawIcon(dl, icon, { x + iw * 0.5f, bb.GetCenter().y }, iw, u32(tone));
        dl->AddText(f, f->FontSize, { x + iw + px(6), std::round(bb.GetCenter().y - t.y * 0.5f) }, u32(tone), label);
        focusRing(bb, gid, px(8));
        recordLayout(id, bb);
        if (hovered && input::tracker().lastWasMouse() && GImGui->HoveredIdTimer > 0.5f) ImGui::SetTooltip("%s\n%s", tooltip, url);
    }
    // One rail entry: selection marker, hover/selection fill, focus ring.
    // Pad/keyboard focus selects (console style); a click selects too.
    template <class Draw, class Select>
    void item(const char* id, ImVec2 min, ImVec2 max, bool selected, Draw draw, Select select, const char* tooltip) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImGuiID gid = window->GetID(id);
        const ImRect bb(min, max);
        ImGui::SetCursorScreenPos(min);
        ImGui::ItemSize(bb.GetSize());
        if (!ImGui::ItemAdd(bb, gid)) return;
        m_ids.push_back(gid);
        bool hovered = false, held = false;
        const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
        const bool mouse = input::tracker().lastWasMouse();
        if (pressed) {
            select();
            // A or Enter on the rail enters the page.
            if (!mouse) requestFocus();
        } else if (GImGui->NavJustMovedToId == gid && m_wasInRail && (m_navKey || navKeyDown()) && !mouse && !selected && !m_focusRail)
            select();
        if (m_focusRail && selected) {
            setFocus(gid, window, !mouse);
            focusRequested() = false;
        }
        auto* dl = window->DrawList;
        if (selected) {
            dl->AddRectFilled(bb.Min, bb.Max, ink(color::raised), px(10));
            dl->AddRectFilled({ bb.Min.x, bb.Min.y + px(10) }, { bb.Min.x + px(3), bb.Max.y - px(10) }, ink(color::cta), px(2));
        } else if (hovered)
            dl->AddRectFilled(bb.Min, bb.Max, ink(color::surface), px(10));
        draw(dl, bb, selected, hovered);
        focusRing(bb, gid, px(10));
        recordLayout(tooltip ? tooltip : id, bb);
        if (tooltip && hovered && mouse && GImGui->HoveredIdTimer > 0.5f) ImGui::SetTooltip("%s", tooltip);
    }
    // A directional key or pad direction was pressed this frame, so a focus
    // change is the player moving (not ImGui's initial focus pick).
    static bool navKeyDown() {
        for (ImGuiKey key : { ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadUp,
                              ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickUp,
                              ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ImGuiKey_Tab })
            if (ImGui::IsKeyPressed(key, true)) return true;
        return false;
    }
    bool m_navKey = false, m_wasInRail = false;
    struct Toast {
        std::string text;
        double shown;
        bool ok;
    };
    std::deque<Toast> m_toasts;
    std::vector<ImGuiID> m_ids;
    bool m_focusRail = false;
};

// ---------------------------------------------------------------- About
inline void aboutPage() {
    titleBar("About", false);
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const bool wide = area.x >= px(820);
    const float leftW = wide ? std::floor((area.x - px(24)) * 0.46f) : area.x;
    beginCard("about-intro", { leftW, 0 }, 28);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float logoH = px(120);
        drawGlow(ImGui::GetWindowDrawList(), { p.x + logoH * images().logoAspect * 0.5f, p.y + logoH * 0.5f }, logoH * 0.62f, logoH * 0.6f);
        ImFont* f = brandWidth(logoH, fonts().display) <= ImGui::GetContentRegionAvail().x ? fonts().display : fonts().title;
        drawBrand(ImGui::GetWindowDrawList(), p, logoH, f);
        recordLayout("about-brand", ImRect(p, { p.x + brandWidth(logoH, f), p.y + logoH }));
        ImGui::Dummy({ brandWidth(logoH, f), logoH });
    }
    gap(8);
    textWrapped(brand::credit, fonts().body, color::text);
    gap(2);
    textWrapped("Sly Cooper and the Thievius Raccoonus and Sly 2: Band of Thieves are fully playable. "
                "Sly 3: Honor Among Thieves is in early development and not fully playable yet: expect missing features and crashes.",
                fonts().caption, color::text2);
    divider(14);
    heading("Community");
    caption("News, help and the upcoming ThiefRush multiplayer fork.", color::text2);
    gap(2);
    if (button(brand::discordLabel, Tone::Secondary, { 0, 0 }, Icon::Chat)) openLink(brand::discordUrl);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", brand::discordUrl);
    if (button(brand::patreonLabel, Tone::Secondary, { 0, 0 }, Icon::Heart)) openLink(brand::patreonUrl);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", brand::patreonUrl);
    endCard();
    if (wide) ImGui::SameLine(0, px(24));
    else gap(4);
    beginCard("about-credits", { wide ? area.x - leftW - px(24) : area.x, 0 }, 24);
    heading("Credits");
    gap(2);
    auto credit = [](const char* role, const char* who, ImTextureID image = nullptr) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        auto* dl = ImGui::GetWindowDrawList();
        float x = p.x;
        if (image) {
            dl->AddRectFilled(p, { p.x + px(48), p.y + px(48) }, ink(color::raised), px(10));
            dl->AddImage(image, { p.x + px(4), p.y + px(4) }, { p.x + px(44), p.y + px(44) });
            x += px(62);
        }
        dl->AddText(fonts().caption, fonts().caption->FontSize, { x, p.y + px(2) }, ink(color::muted), role);
        const float wrap = ImGui::GetContentRegionAvail().x - (x - p.x);
        dl->AddText(fonts().body, fonts().body->FontSize, { x, p.y + px(22) }, ink(color::text), who, nullptr, wrap);
        const auto t = fonts().body->CalcTextSizeA(fonts().body->FontSize, FLT_MAX, wrap, who);
        ImGui::Dummy({ 1, std::max(image ? px(48) : 0.0f, px(22) + t.y) });
        recordLayout(role, ImRect(p, { p.x + wrap, p.y + px(22) + t.y }));
    };
    credit("Sly Cooper and the Thievius Raccoonus port", brand::projectCaneCredit);
    credit("SafeHouse logo", brand::logoCredit, images().logo);
    credit("App icon", brand::iconCredit, images().icon);
    credit("Steam library artwork", "SteamGridDB artists: ctwoafiveb, Benuno, AnimeFan69, Spaghetti Overlord, Pedrogon18, "
                                    "RealSayakaMaizono, Nuke, simenfje, FrostWolf (share/safehouse/steam/CREDITS.md)");
    credit("Controller prompts", "Kenney Input Prompts (CC0) · kenney.nl");
    credit("Controller database", "SDL_GameControllerDB (zlib licence)");
    credit("Typeface", "Atkinson Hyperlegible by the Braille Institute (SIL Open Font License)");
    divider(14);
    caption("Full licence texts ship in the licenses folder and THIRD_PARTY_NOTICES.md.", color::text2);
    caption("Sly Cooper, its sequels, Bentley's Hackpack and their logos are trademarks of Sony Interactive Entertainment. "
            "The game logos identify the games only. SafeHouse is an unofficial fan project, not affiliated with or endorsed by "
            "Sony Interactive Entertainment, Sucker Punch Productions or Sanzaru Games. Controller names and symbols belong to their owners.");
    const fs::path licences = executableDirectory().parent_path() / "licenses";
    std::error_code ec;
    if (fs::is_directory(licences, ec)) {
        gap(2);
        if (button("Open licences", Tone::Quiet, { 0, 0 }, Icon::Folder)) try {
                openFolder(licences);
            } catch (...) {
            }
        takeFocus(!input::tracker().lastWasMouse());
    }
    endCard();
}
} // namespace cane::ui
