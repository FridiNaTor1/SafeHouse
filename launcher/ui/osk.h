#pragma once
// Pad-friendly text entry: an on-screen keyboard for ImGui text fields.
//
// padInputText() is an ImGui::InputText that also opens a modal keyboard when
// the field is confirmed with the pad (South; East for Nintendo) while a pad was the last device
// used. The keyboard edits a copy of the text; Done hands it back to the field
// on the next frame (padInputText() then returns true), Cancel drops it.
// In the keyboard: D-pad/left stick move, A types, X deletes, Y types a space,
// LB/RB move the text cursor, L3 shifts, Start is Done and B cancels (A and
// B follow the Nintendo convention there, see face_buttons.h). A
// physical keyboard (or Steam's, Steam + X) types into it too: Enter accepts
// and Escape cancels. Typed text obeys the field's Charset and its
// ImGuiInputTextFlags_Chars* flags, exactly like ImGui's own filtering.
//
// The keyboard popup is opened and drawn by its field, at the field's popup
// level, so a field inside a modal keeps that modal open. If another popup
// replaces the keyboard (an OpenPopup() at the same level), the keyboard steps
// aside with its text kept and comes back once that popup has closed. It is
// cancelled when its field is no longer drawn, disabled or read-only.
// Requires GamepadNav (gamepad_nav.h) to run each frame and
// drawOnScreenKeyboard() once per frame after all screens.
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include "controller_family.h"
#include "face_buttons.h"
#include "gamepad_nav.h"
#include "prompt_bar.h"
namespace cane::launcher {
// Allowed characters. Password previews masked.
enum class Charset { Name, Password, Free };

namespace detail {
// The character to insert for a typed code point, or 0 to reject it.
inline unsigned charsetFilter(Charset, unsigned c) {
    if (c < 32 || c == 127 || (c >= 0x80 && c < 0xA0) || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return 0;
    return c;
}
inline int inputTextFilter(ImGuiInputTextCallbackData* data) {
    const unsigned mapped = charsetFilter(*static_cast<const Charset*>(data->UserData), data->EventChar);
    if (!mapped) return 1;
    data->EventChar = static_cast<ImWchar>(mapped);
    return 0;
}
inline std::string utf8(unsigned c) {
    std::string out;
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    return out;
}
// The flags InputText honours when filtering typed characters.
inline constexpr ImGuiInputTextFlags kCharFlags = ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_CharsHexadecimal |
                                                  ImGuiInputTextFlags_CharsScientific | ImGuiInputTextFlags_CharsUppercase |
                                                  ImGuiInputTextFlags_CharsNoBlank;
// ImGui's InputTextFilterCharacter() for typed (not pasted) single-line text:
// the character to insert, or 0 to reject it. decimalPoint is
// ImGuiIO::PlatformLocaleDecimalPoint.
inline unsigned inputFlagsFilter(ImGuiInputTextFlags flags, unsigned c, unsigned decimalPoint = '.') {
    if (c < 0x20 || c == 127 || (c >= 0xE000 && c <= 0xF8FF) || c > IM_UNICODE_CODEPOINT_MAX) return 0;
    if (!(flags & kCharFlags)) return c;
    const bool numeric = flags & (ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_CharsScientific);
    if (numeric && (c == '.' || c == ',')) c = decimalPoint;
    if ((flags & (ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_CharsScientific | ImGuiInputTextFlags_CharsHexadecimal)) &&
        c >= 0xFF01 && c <= 0xFF5E)
        c = c - 0xFF01 + 0x21; // full-width to half-width
    const bool digit = c >= '0' && c <= '9';
    const bool arithmetic = c == decimalPoint || c == '-' || c == '+' || c == '*' || c == '/';
    if ((flags & ImGuiInputTextFlags_CharsDecimal) && !digit && !arithmetic) return 0;
    if ((flags & ImGuiInputTextFlags_CharsScientific) && !digit && !arithmetic && c != 'e' && c != 'E') return 0;
    if ((flags & ImGuiInputTextFlags_CharsHexadecimal) && !digit && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F'))
        return 0;
    if ((flags & ImGuiInputTextFlags_CharsUppercase) && c >= 'a' && c <= 'z') c -= 32;
    if ((flags & ImGuiInputTextFlags_CharsNoBlank) && (c == ' ' || c == '\t' || c == 0x3000)) return 0;
    return c;
}
inline bool utf8Continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
// Cuts text to at most limit bytes without splitting a UTF-8 sequence.
inline void utf8Truncate(std::string& text, size_t limit) {
    if (text.size() <= limit) return;
    size_t end = limit;
    while (end > 0 && utf8Continuation(text[end])) --end;
    text.resize(end);
}

// The text being edited: UTF-8, a byte cursor on a code point boundary and a
// byte limit (the field's capacity minus the terminator). Typed characters go
// through the field's Chars* flags first and then its Charset, in the order
// InputText applies its named filters and then the Charset callback.
struct OskEditor {
    std::string text;
    size_t cursor = 0, limit = 0;
    Charset charset = Charset::Free;
    ImGuiInputTextFlags flags = 0; // the field's ImGuiInputTextFlags_Chars* flags
    unsigned decimalPoint = '.';
    void reset(std::string_view initial, size_t capacity, Charset set, ImGuiInputTextFlags fieldFlags = 0) {
        limit = capacity ? capacity - 1 : 0;
        charset = set;
        flags = fieldFlags & kCharFlags;
        decimalPoint = ImGui::GetCurrentContext() ? ImGui::GetIO().PlatformLocaleDecimalPoint : '.';
        text.assign(initial.substr(0, initial.find('\0')));
        utf8Truncate(text, limit);
        cursor = text.size();
    }
    bool upperCase() const { return flags & ImGuiInputTextFlags_CharsUppercase; }
    // The character a typed code point becomes, or 0 when the field rejects it.
    unsigned filter(unsigned codepoint) const {
        const unsigned c = inputFlagsFilter(flags, codepoint, decimalPoint);
        return c ? charsetFilter(charset, c) : 0;
    }
    bool accepts(unsigned codepoint) const { return filter(codepoint) != 0; }
    bool insert(unsigned codepoint) {
        const unsigned c = filter(codepoint);
        if (!c) return false;
        const std::string bytes = utf8(c);
        if (text.size() + bytes.size() > limit) return false;
        text.insert(cursor, bytes);
        cursor += bytes.size();
        return true;
    }
    bool backspace() {
        if (cursor == 0) return false;
        size_t start = cursor - 1;
        while (start > 0 && utf8Continuation(text[start])) --start;
        text.erase(start, cursor - start);
        cursor = start;
        return true;
    }
    void left() {
        if (cursor == 0) return;
        --cursor;
        while (cursor > 0 && utf8Continuation(text[cursor])) --cursor;
    }
    void right() {
        if (cursor >= text.size()) return;
        ++cursor;
        while (cursor < text.size() && utf8Continuation(text[cursor])) ++cursor;
    }
};

// Key layouts. Each row is a run of single-character keys; the bottom row of
// action keys depends on the charset.
struct OskLayout {
    std::array<std::string_view, 4> rows;
    bool letters; // Shift applies
};
inline constexpr OskLayout kOskLetters{ { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-" }, true };
inline constexpr OskLayout kOskSymbols{ { "1234567890", "!@#$%^&*()", "_=+[]{};:\"", "/?\\|~`<>,." }, false };
// Number pads for ImGuiInputTextFlags_CharsDecimal / _CharsScientific /
// _CharsHexadecimal fields (a port, say). Empty rows are skipped.
inline constexpr OskLayout kOskDecimal{ { "123", "456", "789", "-0." }, false };
inline constexpr OskLayout kOskScientific{ { "123+", "456-", "789e", ".0" }, false };
inline constexpr OskLayout kOskHexadecimal{ { "1234567890", "abcdef", "", "" }, true };
inline const OskLayout& oskLayout(const OskEditor& editor, bool symbols) {
    if (editor.flags & ImGuiInputTextFlags_CharsDecimal) return kOskDecimal;
    if (editor.flags & ImGuiInputTextFlags_CharsScientific) return kOskScientific;
    if (editor.flags & ImGuiInputTextFlags_CharsHexadecimal) return kOskHexadecimal;
    return symbols ? kOskSymbols : kOskLetters;
}
// The letters/symbols pages (Shift, &123 and Space keys) are for free text.
inline bool oskPaged(const OskLayout& layout) { return &layout == &kOskLetters || &layout == &kOskSymbols; }

inline constexpr const char* kOskPopup = "##cane_on_screen_keyboard";
struct OskState {
    bool open = false, requestOpen = false, appearing = false;
    // Hosted: opened by its field, which opens and draws the popup at its own
    // popup level every frame. Otherwise drawOnScreenKeyboard() does, at root.
    bool hosted = false;
    // Replaced by another popup at its level; reopens with the text kept.
    bool suspended = false;
    int drawnFrame = -1, popupLevel = 0;
    ImGuiID popupId = 0;
    // Root style and font from drawOnScreenKeyboard(), so a hosted keyboard
    // does not inherit style pushed around its field.
    ImGuiStyle baseStyle;
    int baseFontDepth = 0; // g.FontStack size at root
    bool haveBase = false;
    ImGuiID owner = 0;
    std::string title;
    OskEditor editor;
    bool password = false, symbols = false;
    int shift = 0; // 0 off, 1 next letter, 2 caps lock
    // Result handed to the owning field on its next padInputText() call.
    ImGuiID resultFor = 0;
    bool resultReady = false;
    std::string result;
};
inline OskState& oskState() {
    static OskState state;
    return state;
}
inline std::string_view visibleLabel(const char* label) {
    std::string_view text = label ? label : "";
    return text.substr(0, text.find("##"));
}
// Opens the keyboard for a field (also used by tests and screenshots).
inline void openOnScreenKeyboard(ImGuiID owner, const char* label, const char* text, size_t capacity, Charset charset,
                                 ImGuiInputTextFlags flags = 0) {
    auto& osk = oskState();
    osk.owner = owner;
    osk.title = visibleLabel(label);
    if (osk.title.empty()) osk.title = charset == Charset::Password ? "Password" : "Enter text";
    osk.editor.reset(text ? text : "", capacity, charset, flags);
    osk.password = charset == Charset::Password || (flags & ImGuiInputTextFlags_Password);
    osk.symbols = false;
    osk.shift = 0;
    osk.requestOpen = true;
    osk.open = osk.suspended = osk.hosted = false;
    osk.resultReady = false;
}
inline void finishOnScreenKeyboard(bool accept) {
    auto& osk = oskState();
    if (accept) {
        osk.resultFor = osk.owner;
        osk.result = osk.editor.text;
        osk.resultReady = true;
    }
    osk.open = osk.suspended = false;
    padFrame().oskOpen = false;
    ImGui::CloseCurrentPopup();
}
// Drops the keyboard (and its popup, if still open) without a result.
inline void cancelOnScreenKeyboard() {
    auto& osk = oskState();
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (osk.open && g && g->OpenPopupStack.Size > osk.popupLevel &&
        g->OpenPopupStack[osk.popupLevel].PopupId == osk.popupId)
        ImGui::ClosePopupToLevel(osk.popupLevel, true);
    osk.open = osk.requestOpen = osk.suspended = osk.hosted = false;
    padFrame().oskOpen = false;
}
inline bool showSteamKeyboardHint() {
    const input::Family family = launcherPromptFamily();
    return family == input::Family::SteamDeck || family == input::Family::SteamController ||
           family == input::Family::SteamController2026 ||
           std::getenv("SteamDeck") || std::getenv("SteamAppId") || std::getenv("SteamGameId") ||
           std::getenv("SteamVirtualGamepadInfo");
}
// Opens the keyboard popup when requested and draws it, at the current popup
// level: inside the field's own popups when hosted, at root otherwise.
inline void runKeyboard() {
    auto& osk = oskState();
    auto& frame = padFrame();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    osk.drawnFrame = g.FrameCount;
    // Stepped aside for another popup: come back once this level is free.
    if (osk.suspended && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        osk.suspended = false;
        osk.requestOpen = true;
    }
    if (osk.requestOpen) {
        ImGui::OpenPopup(kOskPopup);
        osk.popupLevel = g.BeginPopupStack.Size;
        osk.popupId = ImGui::GetID(kOskPopup);
        osk.requestOpen = false;
        osk.open = osk.appearing = true;
    }
    frame.oskOpen = osk.open;
    if (!osk.open) return;
    // A hosted keyboard draws with the root style and font and no item flags,
    // not whatever the screen pushed around its field.
    const bool rebase = osk.hosted && osk.haveBase;
    ImGuiStyle fieldStyle;
    if (rebase) {
        fieldStyle = g.Style;
        g.Style = osk.baseStyle;
        ImGui::PushFont(osk.baseFontDepth > 0 && osk.baseFontDepth <= g.FontStack.Size ? g.FontStack[osk.baseFontDepth - 1]
                                                                                        : ImGui::GetDefaultFont());
    }
    if (osk.hosted) ImGui::PushItemFlag(g.CurrentItemFlags, false);
    struct Restore {
        ImGuiContext& g;
        bool rebase, hosted;
        ImGuiStyle& style;
        ~Restore() {
            if (hosted) ImGui::PopItemFlag();
            if (rebase) {
                ImGui::PopFont();
                g.Style = style;
            }
        }
    } restore{ g, rebase, osk.hosted, fieldStyle };

    ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float font = ImGui::GetFontSize();
    const float width = std::min(io.DisplaySize.x - 32.0f, std::max(640.0f, font * 46.0f));
    const float pad = std::round(font * 1.2f), gap = std::round(font * 0.4f);
    const OskLayout& layout = oskLayout(osk.editor, osk.symbols);
    size_t columns = 0;
    for (std::string_view row : layout.rows) columns = std::max(columns, row.size());
    columns = columns <= 4 ? 6 : 10; // number pads get wider keys
    const float keyWidth = std::floor((width - pad * 2 - gap * (columns - 1)) / columns);
    const float keyHeight = std::round(std::clamp(font * 2.3f, 40.0f, 60.0f));
    const float iconPx = std::round(font * 1.5f);
    ImGui::SetNextWindowPos({ io.DisplaySize.x * 0.5f, io.DisplaySize.y - 16.0f }, ImGuiCond_Always, { 0.5f, 1.0f });
    ImGui::SetNextWindowSizeConstraints({ width, 0.0f }, { width, io.DisplaySize.y - 32.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { pad, pad });
    const bool visible = ImGui::BeginPopupModal(kOskPopup, nullptr,
                                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    if (!visible) {
        // Another popup replaced it at this level: step aside, keep the text.
        osk.open = frame.oskOpen = false;
        osk.suspended = true;
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { gap, gap });
    auto& editor = osk.editor;
    const bool appearing = osk.appearing;
    osk.appearing = false;
    const bool keyboardUsed = !input::tracker().lastWasGamepad() && !input::tracker().lastWasMouse();
    const bool upper = osk.shift != 0;
    auto typeText = [&](unsigned c) {
        // One-shot shift ends after a letter (typed upper-case).
        if (editor.insert(c) && osk.shift == 1 && (c | 0x20) >= 'a' && (c | 0x20) <= 'z') osk.shift = 0;
    };
    auto applies = [&] { return !keyboardUsed; }; // on-screen keys ignore keyboard-triggered presses
    bool finish = false, accept = false;

    // Title and preview with caret.
    ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
    ImGui::TextUnformatted(osk.title.c_str());
    ImGui::PopStyleColor();
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max{ min.x + ImGui::GetContentRegionAvail().x, min.y + std::round(keyHeight * 1.2f) };
        drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
        drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_NavHighlight), style.FrameRounding, 0, 2.0f);
        std::string shown = editor.text;
        size_t caret = editor.cursor;
        if (osk.password) {
            size_t glyphs = 0, before = 0;
            for (size_t i = 0; i < editor.text.size(); ++i)
                if (!utf8Continuation(editor.text[i])) {
                    ++glyphs;
                    if (i < editor.cursor) ++before;
                }
            shown.assign(glyphs, '*');
            caret = before;
        }
        const float inner = font * 0.6f, available = max.x - min.x - inner * 2;
        const float caretX = ImGui::CalcTextSize(shown.c_str(), shown.c_str() + caret).x;
        const float scroll = std::max(0.0f, caretX - available + 2.0f);
        const ImVec2 textPos{ min.x + inner - scroll, min.y + (max.y - min.y - font) * 0.5f };
        drawList->PushClipRect({ min.x + inner * 0.5f, min.y }, { max.x - inner * 0.5f, max.y }, true);
        if (shown.empty()) {
            (void)editor;
        } else
            drawList->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());
        if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) {
            const float x = std::round(textPos.x + caretX);
            drawList->AddLine({ x, textPos.y - 2 }, { x, textPos.y + font + 2 }, ImGui::GetColorU32(ImGuiCol_Text), 2.0f);
        }
        drawList->PopClipRect();
        ImGui::Dummy({ max.x - min.x, max.y - min.y });
    }
    ImGui::Dummy({ 1.0f, gap });

    // Character keys.
    const float rowLeft = ImGui::GetCursorPosX();
    for (size_t row = 0; row < layout.rows.size(); ++row) {
        const std::string_view keys = layout.rows[row];
        if (keys.empty()) continue;
        const float rowWidth = keys.size() * keyWidth + (keys.size() - 1) * gap;
        ImGui::SetCursorPosX(rowLeft + std::round((width - pad * 2 - rowWidth) * 0.5f));
        for (size_t i = 0; i < keys.size(); ++i) {
            char c = keys[i];
            if (layout.letters && upper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
            if (editor.upperCase() && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
            // Number pads show (and type) the locale's decimal point.
            if (c == '.' && !oskPaged(layout) && editor.decimalPoint < 128) c = static_cast<char>(editor.decimalPoint);
            // A fixed ID survives Shift and page changes, so focus stays put;
            // the character is drawn separately ('#' cannot be a label).
            ImGui::PushID(static_cast<int>(row * 64 + i));
            if (i) ImGui::SameLine();
            const bool pressed = ImGui::Button("##key", { keyWidth, keyHeight });
            const char text[2] = { c, 0 };
            const ImVec2 textSize = ImGui::CalcTextSize(text), itemMin = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText({ std::round(itemMin.x + (keyWidth - textSize.x) * 0.5f),
                                                  std::round(itemMin.y + (keyHeight - textSize.y) * 0.5f) },
                                                ImGui::GetColorU32(ImGuiCol_Text), text);
            if (pressed && applies()) typeText(static_cast<unsigned char>(c));
            if (appearing && row == 1 && i == 0) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
    }
    // Action row.
    {
        struct Action {
            const char* label;
            float units;
            int id;
        };
        enum { Shift, Page, Space, OpenBracket, CloseBracket, Delete, Done };
        Action actions[6];
        int count = 0;
        if (oskPaged(layout)) {
            actions[count++] = { osk.symbols ? "Shift" : osk.shift == 2 ? "Caps" : "Shift", 1.5f, Shift };
            actions[count++] = { osk.symbols ? "ABC" : "&123", 1.5f, Page };
            if (editor.accepts(' ')) actions[count++] = { "Space", 3.0f, Space };
        }
        actions[count++] = { "Delete", 2.0f, Delete };
        actions[count++] = { "Done", 2.0f, Done };
        float units = 0;
        for (int i = 0; i < count; ++i) units += actions[i].units;
        const float unit = (width - pad * 2 - gap * (count - 1)) / units;
        ImGui::SetCursorPosX(rowLeft);
        for (int i = 0; i < count; ++i) {
            const Action& action = actions[i];
            if (i) ImGui::SameLine();
            const bool done = action.id == Done;
            const bool active = action.id == Shift && osk.shift != 0 && !osk.symbols;
            if (done || active) {
                const ImVec4 accent = style.Colors[ImGuiCol_NavHighlight];
                ImGui::PushStyleColor(ImGuiCol_Button, accent);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x * 1.12f, accent.y * 1.12f, accent.z * 1.12f, 1));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x * 0.85f, accent.y * 0.85f, accent.z * 0.85f, 1));
                ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_WindowBg]);
            }
            ImGui::BeginDisabled(action.id == Shift && osk.symbols);
            ImGui::PushID(action.id);
            const std::string label = std::string(action.label) + "###action";
            const bool pressed = ImGui::Button(label.c_str(), { std::floor(unit * action.units), keyHeight }) && applies();
            ImGui::PopID();
            ImGui::EndDisabled();
            if (done || active) ImGui::PopStyleColor(4);
            if (!pressed) continue;
            switch (action.id) {
            case Shift: osk.shift = (osk.shift + 1) % 3; break;
            case Page: osk.symbols = !osk.symbols; break;
            case Space: typeText(' '); break;
            case OpenBracket: typeText('['); break;
            case CloseBracket: typeText(']'); break;
            case Delete: editor.backspace(); break;
            case Done: finish = accept = true; break;
            default: break;
            }
        }
    }

    // Pad shortcuts (raw edges; screens receive none while this is open).
    const PadEdges& edges = frame.edges;
    if (frame.live) {
        if (edges.x) editor.backspace();
        if (edges.y && editor.accepts(' ')) typeText(' ');
        if (edges.lb) editor.left();
        if (edges.rb) editor.right();
        if (frame.l3 && layout.letters) osk.shift = osk.shift ? 0 : 1;
        if (edges.start) finish = accept = true;
        // Cancel is the back button: east, or south for Nintendo (where east
        // is A and types through ImGui).
        if (ui::eastConfirms(ui::faceButtonFamily()) ? frame.south : edges.back) {
            finish = true;
            accept = false;
        }
    }
    // Physical keyboard (including Steam's): characters type, Backspace
    // deletes, Enter accepts and Escape cancels.
    if (keyboardUsed && !finish) {
        for (ImWchar c : io.InputQueueCharacters) typeText(c);
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) editor.backspace();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) finish = true;
        else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
            finish = accept = true;
    }

    // Hints and the Steam keyboard note.
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float top = ImGui::GetCursorScreenPos().y + gap;
        const float right = ImGui::GetWindowPos().x + width - pad;
        const bool space = editor.accepts(' ');
        const Hint hints[] = {
            { ui::confirmButton(), 0, "Type" },
            { input::Button::West, 0, "Delete" },
            { space ? input::Button::North : input::Button::Count, 0, "Space" },
            { input::Button::LB, 0, "" },
            { input::Button::RB, 0, "Move" },
            { layout.letters ? input::Button::LSPress : input::Button::Count, 0, "Shift" },
            { input::Button::Start, 0, "Done" },
            { ui::backButton(), 0, "Cancel" },
        };
        drawPromptHints(drawList, { right, top + iconPx * 0.5f }, iconPx, hints, ImGui::GetColorU32(ImGuiCol_Text),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), launcherPromptFamily());
        ImGui::Dummy({ 1.0f, iconPx + gap });
        if (showSteamKeyboardHint()) {
            ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
            ImGui::TextUnformatted("Steam + X also opens Steam's keyboard");
            ImGui::PopStyleColor();
        }
    }
    ImGui::PopStyleVar();
    if (finish) finishOnScreenKeyboard(accept);
    ImGui::EndPopup();
}
inline bool padInputTextImpl(const char* label, const char* hint, char* buf, size_t cap, Charset charset,
                             ImGuiInputTextFlags flags) {
    auto& osk = oskState();
    const ImGuiID id = ImGui::GetID(label);
    bool changed = false;
    if (osk.resultReady && osk.resultFor == id) {
        std::string text = osk.result;
        utf8Truncate(text, cap ? cap - 1 : 0);
        if (cap) {
            changed = std::strncmp(buf, text.c_str(), cap) != 0;
            std::memcpy(buf, text.c_str(), text.size() + 1);
        }
        osk.resultReady = false;
    }
    if (charset == Charset::Password) flags |= ImGuiInputTextFlags_Password;
    Charset filter = charset;
    const auto callback = (flags & ImGuiInputTextFlags_CallbackCharFilter) ? &inputTextFilter : nullptr;
    changed |= hint ? ImGui::InputTextWithHint(label, hint, buf, cap, flags, callback, &filter)
                    : ImGui::InputText(label, buf, cap, flags, callback, &filter);
    // ImGui only starts text input from the pad with North, which the launcher
    // does not feed; the confirm button on a focused field opens the keyboard
    // instead (South, or East for Nintendo, see face_buttons.h).
    const auto& frame = padFrame();
    const bool confirm = ui::eastConfirms(ui::faceButtonFamily()) ? frame.edges.back : frame.south;
    if (frame.live && confirm && !osk.open && !osk.requestOpen && !ImGui::IsItemActive() &&
        ImGui::IsItemFocused() && input::tracker().lastWasGamepad() && !(flags & ImGuiInputTextFlags_ReadOnly)) {
        openOnScreenKeyboard(id, label, buf, cap, charset, flags);
        osk.hosted = true;
    }
    // The field hosts its keyboard, so the popup opens at the field's popup
    // level (a field inside a modal keeps that modal open). Once per frame,
    // should a screen submit the same field twice.
    if (osk.hosted && osk.owner == id && (osk.open || osk.requestOpen || osk.suspended) &&
        osk.drawnFrame != GImGui->FrameCount) {
        if ((GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) || (flags & ImGuiInputTextFlags_ReadOnly))
            cancelOnScreenKeyboard();
        else
            runKeyboard();
    }
    return changed;
}
} // namespace detail

// Same contract as ImGui::InputText (returns true when the text changed).
// With a pad as the last device, confirming the field opens the keyboard,
// which applies the same ImGuiInputTextFlags_Chars* filters (CharsDecimal and
// friends get a number pad). The field may live inside a popup or modal.
inline bool padInputText(const char* label, char* buf, size_t cap, Charset charset, ImGuiInputTextFlags flags = 0) {
    return detail::padInputTextImpl(label, nullptr, buf, cap, charset, flags);
}
// ImGui::InputTextWithHint counterpart.
inline bool padInputTextWithHint(const char* label, const char* hint, char* buf, size_t cap, Charset charset,
                                 ImGuiInputTextFlags flags = 0) {
    return detail::padInputTextImpl(label, hint, buf, cap, charset, flags);
}
// True while the on-screen keyboard owns pad input (screens skip their own
// back/tab handling then; GamepadNav already returns empty edges). False while
// it has stepped aside for another popup.
inline bool onScreenKeyboardOpen() { return detail::oskState().open || detail::oskState().requestOpen; }

// Call once per frame after every screen has been drawn. Cancels a keyboard
// whose field was not drawn this frame and draws one opened with
// detail::openOnScreenKeyboard() outside a field.
inline void drawOnScreenKeyboard() {
    using namespace detail;
    auto& osk = oskState();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    osk.baseStyle = g.Style;
    osk.baseFontDepth = g.FontStack.Size;
    osk.haveBase = true;
    const bool pending = osk.open || osk.requestOpen || osk.suspended;
    if (osk.hosted) {
        if (pending && osk.drawnFrame != g.FrameCount) cancelOnScreenKeyboard();
        padFrame().oskOpen = osk.open;
    } else if (pending)
        runKeyboard();
    else
        padFrame().oskOpen = false;
}
} // namespace cane::launcher
