// SafeHouse on the Switch: SafeHouse.nro, the program the Homebrew Menu lists, the launcher and the games in one.
// main below: the launcher (the games' covers side by side, the one selected pulsing); A starts the game in this
// process (CaneSwitch::RunGame: it installs itself from a disc image at its first start); when it ends, SafeHouse.nro
// starts again, back here. A release build offers a newer SafeHouse.nro from SafeHouse's GitHub releases when the launcher starts (update.h).
//
// The games are Sly1-Release, Sly2-Release and Sly3-Release, each linked into SafeHouse.nro as one object whose only
// global symbol is its descriptor (cane_game_sly1, cane_game_sly2, cane_game_sly3: cmake/Switch.cmake).
//
// The design is the Half-Life 2 Switch port's game picker (NaGaa95/hl2_nx source/picker.c): the covers' layout, the
// pulse, the orange (playable) and red (not) borders, the status lines under the covers and the detail line at the
// top; drawn here with Dear ImGui on the GL layer (switch_ui.h), as the installer is.
#include "update.h"
#include "../runtime.h"
#include "../switch_ui.h"
#include "../../games.h"
#include "../../installer/view.h"
#include "switch_assets.h"
#include "launcher_assets.h"

#include <imgui/imgui.h>
#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The games (Sly1-Release/main.cpp, Sly2-Release/main.cpp, Sly3-Release/main.cpp)
extern "C" const CaneSwitch::Game cane_game_sly1, cane_game_sly2, cane_game_sly3;

namespace
{
using namespace CaneSwitch;

constexpr float kCoverW = 300.0f, kCoverH = 450.0f;   // (450 x 675 at the picker's 1080p, in the 1280 x 720 layout)
constexpr float kPulsePeriod = 2.0f, kPulseScale = 1.12f;
const ImU32 kOrange = IM_COL32(255, 170, 0, 230), kRed = IM_COL32(220, 70, 55, 230);

const Game* FindGame(const char* key)
{
    for (const Game* g : {&cane_game_sly1, &cane_game_sly2, &cane_game_sly3})
        if (key && !std::strcmp(g->key, key)) return g;
    return nullptr;
}

struct GameSlot
{
    const cane::GameInfo* info = nullptr;
    const Game* game = nullptr;
    GLuint cover = 0;
    bool active = false;      // A plays it or installs it
    bool toInstall = false;   // A installs it (its status shown in red, under its cover while selected)
    std::string status, detail;
};

// What the SD card says about a game: its installation (the game's own check).
void Scan(GameSlot& g)
{
    g.game = FindGame(g.info->key);
    const bool installed = g.game && g.game->installed && g.game->installed(GetPaths().base + "/" + g.game->key);
    const bool ported = g.game && g.game->run;
    g.active = g.game && (ported || !installed);
    g.toInstall = g.game && !installed;
    if (!g.game)
    {
        g.status = "NOT PORTED YET";
        g.detail = "NOT PORTED TO THE SWITCH YET";
    }
    else if (!installed)
    {
        g.status = "NOT INSTALLED";
        g.detail = g.info->title;
    }
    else if (ported)
    {
        g.status.clear();   // (playable: no status line)
        g.detail = g.info->title;
    }
    else
    {
        g.status = "INSTALLED";
        g.detail = "INSTALLED: NOT PORTED TO THE SWITCH YET";
    }
}

std::string Upper(std::string s)
{
    for (char& c : s) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}
void Text(ImDrawList* dl, float size, ImVec2 at, ImU32 color, const std::string& text, bool center = false)
{
    ImFont* font = ImGui::GetFont();
    if (center) at.x -= font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x * 0.5f;
    dl->AddText(font, size, at, color, text.c_str());
}
void Outline(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness) { dl->AddRect(a, b, color, 0.0f, 0, thickness); }
bool Inside(ImVec2 p, ImVec2 a, ImVec2 b) { return p.x >= a.x && p.x < b.x && p.y >= a.y && p.y < b.y; }
// The text cut with "..." to fit `width` at `size`.
std::string Fit(float size, std::string text, float width)
{
    ImFont* font = ImGui::GetFont();
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= width) return text;
    while (!text.empty() && font->CalcTextSizeA(size, FLT_MAX, 0.0f, (text + "...").c_str()).x > width) text.pop_back();
    return text + "...";
}
std::string Megabytes(uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / 1048576.0);
    return text;
}

class Launcher
{
public:
    Launcher()
    {
        if (!m_ui.Ok()) return;
        m_background = Ui::Texture(cane_installer_background, cane_installer_background_size);
        const struct { cane::GameId id; const unsigned char* bytes; unsigned long size; } covers[] = {
            {cane::GameId::Sly1, cane_cover_sly1, cane_cover_sly1_size},
            {cane::GameId::Sly2, cane_cover_sly2, cane_cover_sly2_size},
            {cane::GameId::Sly3, cane_cover_sly3, cane_cover_sly3_size},
        };
        for (const auto& c : covers)
        {
            GameSlot g;
            g.info = &cane::game(c.id);
            g.cover = Ui::Texture(c.bytes, c.size);
            Scan(g);
            m_games.push_back(g);
        }
        Update::Check(GetPaths().self);
    }
    ~Launcher()
    {
        Update::Stop();
        for (GameSlot& g : m_games) if (g.cover) glDeleteTextures(1, &g.cover);
        if (m_background) glDeleteTextures(1, &m_background);
    }
    bool Ok() const { return m_ui.Ok(); }

    // Until a game is chosen (it) or the player quits (nullptr). `from`: the game the player came back from, selected
    // (else Sly 1).
    const Game* Run(const char* from)
    {
        m_selected = 0;
        for (size_t i = 0; i < m_games.size(); ++i)
            if (from && !std::strcmp(m_games[i].info->key, from)) m_selected = int(i);
        for (;;)
        {
            if (!m_ui.Begin()) return nullptr;
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - m_last).count();
            m_last = now;
            m_pulse = std::fmod(m_pulse + std::clamp(dt, 0.0f, 0.1f), kPulsePeriod);
            const Update::Status update = Update::Get();
            const bool panel = UpdatePanel(update);
            const int result = panel ? InputUpdate(update) : InputMain();
            Draw();
            if (panel) DrawUpdate(ImGui::GetForegroundDrawList(), update);
            m_ui.End();
            if (result < 0) return nullptr;
            if (result > 0) return m_games[size_t(m_selected)].game;
        }
    }

private:
    ImVec2 CoverPos(size_t i) const
    {
        const float gap = (1280.0f - float(m_games.size()) * kCoverW) / float(m_games.size() + 1);
        return ImVec2(gap + float(i) * (kCoverW + gap), (720.0f - kCoverH) * 0.5f);
    }

    void Move(int direction)
    {
        const int count = int(m_games.size());
        m_selected = (m_selected + (direction > 0 ? 1 : count - 1)) % count;
        m_pulse = 0.0f;
    }

    // 1: the game m_selected was chosen; 0: go on
    int Choose(size_t i)
    {
        if (!m_games[i].active) return 0;
        m_selected = int(i);
        return 1;
    }

    // 1: a game chosen; -1: quit; 0: go on
    int InputMain()
    {
        const u64 pressed = m_ui.Pressed();
        if (pressed & (HidNpadButton_Plus | HidNpadButton_B)) return -1;
        if (pressed & HidNpadButton_AnyLeft) Move(-1);
        if (pressed & HidNpadButton_AnyRight) Move(1);
        if (pressed & HidNpadButton_A) return Choose(size_t(m_selected));
        // touch: a cover starts its game
        if (ImGui::IsMouseClicked(0))
        {
            const ImVec2 m = ImGui::GetIO().MousePos;
            for (size_t i = 0; i < m_games.size(); ++i)
            {
                const ImVec2 p = CoverPos(i);
                if (!Inside(m, p, ImVec2(p.x + kCoverW, p.y + kCoverH))) continue;
                m_selected = int(i);
                m_pulse = 0.0f;
                return Choose(i);
            }
        }
        return 0;
    }

    // ---- the update: a panel over the covers from the check's result to the end of the download
    bool UpdatePanel(const Update::Status& s) const
    {
        switch (s.state)
        {
        case Update::State::Available:
        case Update::State::Failed: return !m_updateDismissed;
        case Update::State::Downloading:
        case Update::State::Installed: return true;
        default: return false;
        }
    }
    static void PanelRect(ImVec2& a, ImVec2& b) { a = ImVec2(320.0f, 245.0f); b = ImVec2(960.0f, 475.0f); }
    // the panel's buttons, centred on its last line (one or two)
    static void ButtonRect(size_t index, size_t count, ImVec2& a, ImVec2& b)
    {
        constexpr float w = 220.0f, h = 46.0f, gap = 30.0f;
        const float left = 640.0f - (float(count) * w + float(count - 1) * gap) * 0.5f + float(index) * (w + gap);
        a = ImVec2(left, 405.0f);
        b = ImVec2(left + w, 405.0f + h);
    }
    // the button touched (its index), else -1
    static int TouchedButton(size_t count)
    {
        if (!ImGui::IsMouseClicked(0)) return -1;
        const ImVec2 m = ImGui::GetIO().MousePos;
        for (size_t i = 0; i < count; ++i)
        {
            ImVec2 a, b;
            ButtonRect(i, count, a, b);
            if (Inside(m, a, b)) return int(i);
        }
        return -1;
    }
    // -1: quit (into the new SafeHouse.nro); 0: go on
    int InputUpdate(const Update::Status& s)
    {
        const u64 pressed = m_ui.Pressed();
        switch (s.state)
        {
        case Update::State::Available:
        {
            const int touched = TouchedButton(2);
            if ((pressed & HidNpadButton_A) || touched == 0) Update::Install();
            else if ((pressed & (HidNpadButton_B | HidNpadButton_Plus)) || touched == 1) m_updateDismissed = true;
            break;
        }
        case Update::State::Downloading:
            if ((pressed & HidNpadButton_B) || TouchedButton(1) == 0)
            {
                Update::Cancel();
                m_updateDismissed = true;
            }
            break;
        case Update::State::Installed:
            if ((pressed & (HidNpadButton_A | HidNpadButton_Plus)) || TouchedButton(1) == 0)
            {
                ChainLoad(GetPaths().self, "");
                return -1;
            }
            break;
        case Update::State::Failed:
            if ((pressed & (HidNpadButton_A | HidNpadButton_B)) || TouchedButton(1) == 0) m_updateDismissed = true;
            break;
        default: break;
        }
        return 0;
    }
    void DrawUpdate(ImDrawList* dl, const Update::Status& s)
    {
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(1280, 720), IM_COL32(0, 0, 0, 150));
        ImVec2 a, b;
        PanelRect(a, b);
        dl->AddRectFilled(a, b, IM_COL32(14, 14, 20, 245));
        Outline(dl, a, b, kOrange, 2.0f);
        const ImU32 title = IM_COL32(255, 205, 80, 255), body = IM_COL32(230, 230, 220, 255);
        const std::string version = "SAFEHOUSE " + Upper(s.version);
        std::vector<const char*> buttons;
        switch (s.state)
        {
        case Update::State::Available:
            Text(dl, 26.0f, ImVec2(640.0f, a.y + 30.0f), title, version + " IS AVAILABLE", true);
            Text(dl, 18.0f, ImVec2(640.0f, a.y + 85.0f), body, "THE NEW SAFEHOUSE.NRO REPLACES THIS ONE.", true);
            Text(dl, 18.0f, ImVec2(640.0f, a.y + 112.0f), body, "YOUR GAMES, SAVES AND SETTINGS STAY AS THEY ARE.", true);
            buttons = {"(A) UPDATE", "(B) LATER"};
            break;
        case Update::State::Downloading:
        {
            Text(dl, 26.0f, ImVec2(640.0f, a.y + 30.0f), title, "DOWNLOADING " + version, true);
            const float fraction = s.total ? std::clamp(float(double(s.done) / double(s.total)), 0.0f, 1.0f) : 0.0f;
            const ImVec2 ba(a.x + 60.0f, a.y + 90.0f), bb(b.x - 60.0f, a.y + 112.0f);
            dl->AddRectFilled(ba, bb, IM_COL32(40, 40, 48, 255));
            dl->AddRectFilled(ba, ImVec2(ba.x + (bb.x - ba.x) * fraction, bb.y), kOrange);
            Text(dl, 18.0f, ImVec2(640.0f, a.y + 125.0f), body, Megabytes(s.done) + " / " + Megabytes(s.total), true);
            buttons = {"(B) CANCEL"};
            break;
        }
        case Update::State::Installed:
            Text(dl, 26.0f, ImVec2(640.0f, a.y + 30.0f), title, version + " IS INSTALLED", true);
            Text(dl, 18.0f, ImVec2(640.0f, a.y + 85.0f), body, "SAFEHOUSE STARTS AGAIN WITH THE NEW VERSION.", true);
            buttons = {"(A) RESTART"};
            break;
        case Update::State::Failed:
            Text(dl, 26.0f, ImVec2(640.0f, a.y + 30.0f), title, "THE UPDATE FAILED", true);
            Text(dl, 17.0f, ImVec2(640.0f, a.y + 85.0f), body, Fit(17.0f, Upper(s.message), b.x - a.x - 60.0f), true);
            Text(dl, 17.0f, ImVec2(640.0f, a.y + 112.0f), body, "THIS SAFEHOUSE.NRO WAS NOT CHANGED.", true);
            buttons = {"(A) OK"};
            break;
        default: break;
        }
        for (size_t i = 0; i < buttons.size(); ++i)
        {
            ImVec2 ba, bb;
            ButtonRect(i, buttons.size(), ba, bb);
            dl->AddRectFilled(ba, bb, IM_COL32(20, 20, 28, 230));
            Outline(dl, ba, bb, i == 0 ? kOrange : IM_COL32(150, 150, 150, 220), i == 0 ? 2.0f : 1.0f);
            Text(dl, 19.0f, ImVec2((ba.x + bb.x) * 0.5f, ba.y + 12.0f), body, buttons[i], true);
        }
    }

    void Draw()
    {
        CaneSetup::Backdrop(m_background ? Ui::Id(m_background) : ImTextureID{}, 215);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float t = m_pulse / kPulsePeriod;
        const float scale = 1.0f + (kPulseScale - 1.0f) * 0.5f * (1.0f - std::cos(t * 6.28318530718f));
        for (size_t i = 0; i < m_games.size(); ++i)
        {
            const GameSlot& g = m_games[i];
            const bool selected = int(i) == m_selected;
            ImVec2 a = CoverPos(i), b(a.x + kCoverW, a.y + kCoverH);
            if (selected && g.active)
            {
                const float w = kCoverW * scale, h = kCoverH * scale;
                a = ImVec2(a.x + (kCoverW - w) * 0.5f, a.y + (kCoverH - h) * 0.5f);
                b = ImVec2(a.x + w, a.y + h);
            }
            if (selected) Outline(dl, ImVec2(a.x - 2.0f, a.y - 2.0f), ImVec2(b.x + 2.0f, b.y + 2.0f), g.active ? kOrange : kRed, 3.0f);
            const ImU32 tint = !g.active ? IM_COL32(65, 65, 65, 190) : selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(155, 155, 155, 255);
            if (g.cover) dl->AddImage(Ui::Id(g.cover), a, b, ImVec2(0, 0), ImVec2(1, 1), tint);
            if (!g.active) dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 115));
            const ImVec2 base = CoverPos(i);
            const ImU32 red = IM_COL32(235, 95, 75, 255);
            const ImU32 statusColor = g.toInstall || !g.active ? red : IM_COL32(255, 185, 80, 255);
            if (!g.status.empty() && (!g.toInstall || selected))
                Text(dl, 21.0f, ImVec2(base.x + kCoverW * 0.5f, 720.0f - 42.0f), statusColor, g.status, true);
        }
        // the selected game's detail at the top
        Text(dl, 21.0f, ImVec2(640.0f, 22.0f), IM_COL32(255, 185, 80, 255), Upper(m_games[size_t(m_selected)].detail), true);
    }

    Ui m_ui;
    GLuint m_background = 0;
    std::vector<GameSlot> m_games;
    int m_selected = 0;
    bool m_updateDismissed = false;
    float m_pulse = 0.0f;
    std::chrono::steady_clock::time_point m_last = std::chrono::steady_clock::now();
};

const Game s_launcher = {"safehouse", "SafeHouse", nullptr, nullptr, nullptr};

// "--name value" out of the command line: the value (nullptr when absent)
const char* TakeArgument(int& argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], name) != 0) continue;
        const char* value = argv[i + 1];
        for (int k = i; k + 2 <= argc; ++k) argv[k] = argv[k + 2];
        argc -= 2;
        return value;
    }
    return nullptr;
}
}  // namespace

// SafeHouse.nro: the launcher, then the game it starts; --game <key> goes straight to a game (SafeHouse.nro restarted
// after installing it), --from <key> is the game the player comes back from (selected).
int main(int argc, char** argv)
{
    const char* requested = TakeArgument(argc, argv, "--game");
    const char* from = TakeArgument(argc, argv, "--from");
    const Game* game = FindGame(requested);
    if (!game)
    {
        if (!Startup(argc, argv, s_launcher))
        {
            ReleaseGraphics();
            Shutdown();
            return 0;
        }
        bool failed = false;
        {
            Launcher launcher;
            if (!launcher.Ok()) failed = true;
            else game = launcher.Run(from);
        }
        if (!game)
        {
            ReleaseGraphics();
            if (failed) ErrorScreen("The graphics could not start.");
            Shutdown();
            return 0;
        }
    }
    // (the launcher's screen stays up: the game takes its window over, as after its installer; a game with its own
    // graphics backend closes it, RunGame)
    return RunGame(*game, argc, argv);
}
