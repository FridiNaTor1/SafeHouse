#pragma once
// The first-boot installer's screens (ProjectCane's platform/installer/view.h): the SD card's folders and disc images,
// the disc's languages to install, the extraction's progress, keep-or-delete the disc image, and what stopped it.
// Platform-neutral Dear ImGui at a 1280x720 layout, driven by platform/switch/installer.cpp; every game's installer
// shares it with its own hint line.
#include <imgui/imgui.h>
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace CaneSetup
{
enum class Screen { Browse, Languages, Extract, Complete, Error };
enum class Action { None, Select, Back, Exit, KeepIso, DeleteIso, Toggle, Install };
struct Entry
{
    std::string name;
    bool folder;
};
struct View
{
    Screen screen = Screen::Browse;
    std::string hint = "Select a Sly Cooper ISO (USA, Europe or Japan).";
    std::string directory = "SD card";
    std::vector<Entry> entries;
    size_t selected = 0;
    size_t firstVisible = 0;
    std::string stage, file, error;
    float progress = 0;
    bool deleteIso = false;
    bool gameReady = false;
    std::vector<std::string> languages;  // (Languages) the disc's, in two columns, and those ticked
    std::vector<bool> chosen;
    size_t language = 0;
    std::string languageNote;            // the space they take
};
inline constexpr size_t ListRows = 6;
inline size_t LanguageRows(const View& view) { return (view.languages.size() + 1) / 2; }
inline void RevealSelection(View& view)
{
    if (view.entries.empty())
    {
        view.selected = view.firstVisible = 0;
        return;
    }
    view.selected = (std::min)(view.selected, view.entries.size() - 1);
    const size_t lastFirst = view.entries.size() > ListRows ? view.entries.size() - ListRows : 0;
    view.firstVisible = (std::min)(view.firstVisible, lastFirst);
    if (view.selected < view.firstVisible + 2)
        view.firstVisible = view.selected > 2 ? view.selected - 2 : 0;
    else if (view.selected >= view.firstVisible + ListRows - 2)
        view.firstVisible = (std::min)(view.selected - (ListRows - 3), lastFirst);
}
inline std::string FitText(const std::string& text, float width)
{
    std::string result;
    for (const unsigned char c : text)
        if (c >= 32) result += char(c);
    if (ImGui::CalcTextSize(result.c_str()).x <= width) return result;
    while (!result.empty() && ImGui::CalcTextSize((result + "...").c_str()).x > width)
    {
        size_t last = result.size() - 1;
        while (last && (static_cast<unsigned char>(result[last]) & 0xc0) == 0x80) --last;
        result.resize(last);
    }
    return result + "...";
}
inline void Heading(const char* text)
{
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted(text);
    ImGui::SetWindowFontScale(1.0f);
}
// The artwork behind every screen: cropped to 16:9 around its centre, darkened (the launcher draws it too).
inline void Backdrop(ImTextureID background, unsigned char shade = 120)
{
    auto* backdrop = ImGui::GetBackgroundDrawList();
    if (background)
        backdrop->AddImage(background, ImVec2(0, 0), ImVec2(1280, 720), ImVec2(0.1389f, 0), ImVec2(0.8611f, 1));
    backdrop->AddRectFilled(ImVec2(0, 0), ImVec2(1280, 720), IM_COL32(7, 14, 27, shade));
}
inline Action Draw(View& view, ImTextureID background)
{
    const ImVec4 accent(0.25f, 0.68f, 1.0f, 1.0f), muted(0.65f, 0.73f, 0.83f, 1.0f);
    Backdrop(background);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(304, 130), ImVec2(976, 610), IM_COL32(0, 0, 0, 72), 24);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 22.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 26));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 10));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.085f, 0.14f, 0.88f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.33f, 0.50f, 0.69f, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.14f, 0.35f, 0.54f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.16f, 0.40f, 0.61f, 1));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.20f, 0.47f, 0.69f, 1));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.19f, 0.28f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.35f, 0.51f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.45f, 0.64f, 1));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
    ImGui::SetNextWindowPos(ImVec2(304, 120));
    ImGui::SetNextWindowSize(ImVec2(672, 480));
    ImGui::Begin("##Setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    Action action = Action::None;
    if (view.screen == Screen::Browse)
    {
        Heading("Install your game");
        ImGui::TextColored(muted, "%s", FitText(view.hint, 610).c_str());
        ImGui::Spacing();
        ImGui::TextColored(accent, "%s", FitText(view.directory, 610).c_str());
        ImGui::Separator();
        RevealSelection(view);
        if (view.entries.empty())
        {
            ImGui::Spacing();
            ImGui::TextColored(muted, "No ISO files in this folder.");
            ImGui::TextColored(muted, "Place your ISO on the SD card to begin.");
        }
        for (size_t i = view.firstVisible; i < (std::min)(view.firstVisible + ListRows, view.entries.size()); ++i)
        {
            ImGui::PushID(int(i));
            const auto& entry = view.entries[i];
            const ImVec2 point = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##Entry", i == view.selected, 0, ImVec2(0, 38)))
            { view.selected = i; action = Action::Select; }
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddText(ImVec2(point.x + 44, point.y + (38 - ImGui::GetFontSize()) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), FitText(entry.name, 562).c_str());
            if (entry.folder)
            {
                draw->AddRectFilled(ImVec2(point.x + 8, point.y + 10), ImVec2(point.x + 20, point.y + 16), IM_COL32(89, 182, 248, 255), 2);
                draw->AddRectFilled(ImVec2(point.x + 8, point.y + 14), ImVec2(point.x + 30, point.y + 29), IM_COL32(89, 182, 248, 255), 3);
            }
            else
            {
                draw->AddRect(ImVec2(point.x + 10, point.y + 8), ImVec2(point.x + 27, point.y + 30), IM_COL32(157, 184, 210, 255), 3, 0, 2);
                draw->AddLine(ImVec2(point.x + 14, point.y + 17), ImVec2(point.x + 23, point.y + 17), IM_COL32(157, 184, 210, 255), 2);
                draw->AddLine(ImVec2(point.x + 14, point.y + 22), ImVec2(point.x + 23, point.y + 22), IM_COL32(157, 184, 210, 255), 2);
            }
            ImGui::PopID();
        }
    }
    else if (view.screen == Screen::Languages)
    {
        Heading("Choose the languages");
        ImGui::TextColored(muted, "%s", FitText("The voices and movie sound to install. The game offers only these.", 610).c_str());
        const size_t rows = LanguageRows(view);
        for (size_t i = 0; i < view.languages.size(); ++i)
        {
            ImGui::PushID(int(i));
            ImGui::SetCursorPos(ImVec2(28 + float(i / rows) * 312, 104 + float(i % rows) * 42));
            const ImVec2 point = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##Language", i == view.language, 0, ImVec2(300, 36)))
            { view.language = i; action = Action::Toggle; }
            auto* draw = ImGui::GetWindowDrawList();
            const ImVec2 box(point.x + 10, point.y + 8);
            if (i < view.chosen.size() && view.chosen[i])
            {
                draw->AddRectFilled(box, ImVec2(box.x + 20, box.y + 20), IM_COL32(64, 173, 255, 255), 4);
                draw->AddLine(ImVec2(box.x + 4, box.y + 10), ImVec2(box.x + 9, box.y + 15), IM_COL32(255, 255, 255, 255), 3);
                draw->AddLine(ImVec2(box.x + 9, box.y + 15), ImVec2(box.x + 17, box.y + 5), IM_COL32(255, 255, 255, 255), 3);
            }
            else draw->AddRect(box, ImVec2(box.x + 20, box.y + 20), IM_COL32(157, 184, 210, 255), 4, 0, 2);
            draw->AddText(ImVec2(point.x + 44, point.y + (36 - ImGui::GetFontSize()) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), FitText(view.languages[i], 246).c_str());
            ImGui::PopID();
        }
        ImGui::SetCursorPosY(362);
        ImGui::TextColored(muted, "%s", FitText(view.languageNote, 610).c_str());
        ImGui::SetCursorPosY(398);
        if (ImGui::Button("B  Back", ImVec2(200, 54))) action = Action::Back;
        ImGui::SameLine();
        ImGui::BeginDisabled(std::find(view.chosen.begin(), view.chosen.end(), true) == view.chosen.end());
        if (ImGui::Button("X  Install", ImVec2(-1, 54))) action = Action::Install;
        ImGui::EndDisabled();
    }
    else if (view.screen == Screen::Extract)
    {
        Heading("Preparing your game");
        ImGui::TextColored(muted, "Keep SafeHouse open until it finishes.");
        ImGui::SetCursorPosY(179);
        ImGui::TextUnformatted(view.stage.c_str());
        ImGui::TextColored(muted, "%s", FitText(view.file, 610).c_str());
        ImGui::Spacing();
        const int percent = int(view.progress * 100);
        ImGui::ProgressBar(view.progress, ImVec2(-1, 16), "");
        ImGui::TextColored(accent, "%d%%", percent);
    }
    else if (view.screen == Screen::Complete)
    {
        Heading("You're ready to play");
        ImGui::TextColored(muted, "Your game files have been extracted.");
        ImGui::SetCursorPosY(186);
        ImGui::TextUnformatted("Delete the ISO to free up space?");
        ImGui::TextColored(muted, "The game will start after your choice.");
        ImGui::SetCursorPosY(316);
        ImGui::PushStyleColor(ImGuiCol_Button, !view.deleteIso ? ImVec4(0.12f, 0.40f, 0.64f, 1) : ImVec4(0.12f, 0.19f, 0.28f, 1));
        if (ImGui::Button("Keep ISO", ImVec2(300, 54))) action = Action::KeepIso;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, view.deleteIso ? ImVec4(0.12f, 0.40f, 0.64f, 1) : ImVec4(0.12f, 0.19f, 0.28f, 1));
        if (ImGui::Button("Delete ISO", ImVec2(300, 54))) action = Action::DeleteIso;
        ImGui::PopStyleColor();
    }
    else
    {
        Heading(view.gameReady ? "Your game is ready" : "Extraction stopped");
        ImGui::SetCursorPosY(141);
        ImGui::PushTextWrapPos(610);
        ImGui::TextUnformatted(view.error.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SetCursorPosY(388);
        if (ImGui::Button(view.gameReady ? "Start game" : "Choose ISO", ImVec2(-1, 54))) action = Action::Back;
    }
    ImGui::End();
    ImGui::PopStyleColor(9);
    ImGui::PopStyleVar(6);
    return action;
}
}
