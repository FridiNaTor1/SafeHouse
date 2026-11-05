// Dynamic controller prompts. See prompt_glyphs.h. From ThiefRush-Dev's
// Sly1-Release prompt_glyphs.cpp (its multiplayer ImGui hints and Vita prompts
// left out); the keyboard keycaps follow Sly2-Native's promptglyph.cpp.
#include "prompt_glyphs.h"
#include "font.h"
#include "gl.h"
#include "save.h"
#include "prompt.h"
#include "../platform/button_prompts.h"
#include "../platform/gamepad_mappings.h"
#include "../platform/system_settings.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace
{
using cane::input::Button;
using cane::input::Family;

// Font 1 (Splash.brx) glyphs that name a game action.
struct GLYPHACTION
{
    char ch;
    JOY_BUTTON button;
};

constexpr GLYPHACTION s_aglyphaction[] =
{
    { 'X', BTN_CROSS }, { 'O', BTN_CIRCLE }, { 'S', BTN_SQUARE }, { 'T', BTN_TRIANGLE },
    { 'L', BTN_L1 }, { 'R', BTN_R1 }, { 'l', BTN_L2 }, { 'r', BTN_R2 },
    { '!', BTN_START }, { '_', BTN_SELECT },
    { '^', BTN_UP }, { 'v', BTN_DOWN }, { '<', BTN_LEFT }, { '>', BTN_RIGHT }
};

std::string KeyLabel(int key)
{
    std::string label = PchzKeyboardKeyName(key);
    // glfwGetKeyName reports printable keys in lower case ("k").
    if (label.size() == 1)
        label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    return label;
}
}

void StartupPromptInput()
{
    cane::input::installJoystickCallback();
    if (!cane::input::applyBundledMappings())
        std::cerr << "Bundled controller mappings were rejected; using GLFW's built-in mappings" << std::endl;
    cane::input::tracker().forceFromEnv();
}

void StartupPromptAtlas()
{
    // Without atlases (or a GL context that cannot take them) every family
    // falls back to the original glyph of the bound button.
    cane::input::promptAtlas().upload();
}

void ShutdownPromptAtlas()
{
    cane::input::promptAtlas().release();
}

Family PromptFamily()
{
    return cane::input::promptFamily(g_promptStyle, g_fAlwaysOriginalPsLayout ? 1 : 0);
}

bool PromptKeyboardActive()
{
    return cane::input::tracker().family() == Family::Keyboard;
}

bool PromptSubstituteActive()
{
    return !g_fAlwaysOriginalPsLayout;
}

JOY_BUTTON JoyButtonFromPromptGlyph(char ch)
{
    for (const GLYPHACTION& glyphaction : s_aglyphaction)
        if (glyphaction.ch == ch)
            return glyphaction.button;
    return BTN_MAX;
}

Button PromptButtonForGlyph(char ch)
{
    // Sticks and the whole d-pad are not remappable.
    switch (ch)
    {
    case '(': return Button::LS;
    case ')': return Button::RS;
    case '+': return Button::Dpad;
    default: break;
    }

    const JOY_BUTTON button = JoyButtonFromPromptGlyph(ch);
    if (button == BTN_MAX)
        return Button::Count;
    return cane::input::fromGlfwBinding(g_gamepadBindings[button]);
}

PROMPTGLYPH PromptGlyphFor(char ch)
{
    PROMPTGLYPH glyph;
    glyph.ch = ch;
    if (!PromptSubstituteActive())
        return glyph;

    const Family family = PromptFamily();
    const auto& atlas = cane::input::promptAtlas();

    // Keyboard: the bound key's keycap (keys the atlas has; else the original
    // glyph). The cell keeps the original glyph's width.
    if (family == Family::Keyboard)
    {
        const JOY_BUTTON button = JoyButtonFromPromptGlyph(ch);
        if (button == BTN_MAX)
            return glyph;
        int key = g_keyboardBindings[button];
        if (button == BTN_START && key == GLFW_KEY_UNKNOWN)
            key = GLFW_KEY_ESCAPE;   // (Escape is always a second Start)
        const cane::input::IconUV uv = atlas.keyUV(cane::input::promptKeyForBinding(key));
        if (!uv.valid || atlas.keyboardTexture() == 0)
            return glyph;
        glyph.kind = PROMPTGLYPH::Icon;
        glyph.texture = atlas.keyboardTexture();
        glyph.u0 = uv.u0;
        glyph.v0 = uv.v0;
        glyph.u1 = uv.u1;
        glyph.v1 = uv.v1;
        return glyph;
    }

    const Button button = PromptButtonForGlyph(ch);
    if (button == Button::Count)
        return glyph;

    // The original glyph of the button actually bound, so remapped controls
    // are shown honestly. Guide has no glyph and keeps the original.
    const char chBound = cane::input::psGlyphFor(button);
    if (chBound != '\0')
    {
        glyph.kind = chBound == ch ? PROMPTGLYPH::Original : PROMPTGLYPH::Glyph;
        glyph.ch = chBound;
    }

    if (family == Family::PlayStation)
        return glyph;

    const GLuint texture = atlas.texture(family);
    const cane::input::IconUV uv = atlas.uv(family, button);
    if (texture == 0 || !uv.valid)
        return glyph;

    glyph.kind = PROMPTGLYPH::Icon;
    glyph.texture = texture;
    glyph.u0 = uv.u0;
    glyph.v0 = uv.v0;
    glyph.u1 = uv.u1;
    glyph.v1 = uv.v1;
    return glyph;
}

void MatchStickGlyphsToWords(std::string& text)
{
    if (!PromptSubstituteActive())
        return;
    // "&2(&." is the left-stick glyph, "&2)&." the right-stick glyph.
    static constexpr const char* s_achzWords[] = { "left ", "right " };
    static constexpr char s_achStick[] = { '(', ')' };
    for (size_t ich = text.find("&2"); ich != std::string::npos; ich = text.find("&2", ich + 2))
    {
        if (ich + 4 >= text.size() || (text[ich + 2] != '(' && text[ich + 2] != ')') ||
            text[ich + 3] != '&' || text[ich + 4] != '.')
            continue;

        for (int iword = 0; iword < 2; ++iword)
        {
            const size_t cch = std::strlen(s_achzWords[iword]);
            if (ich < cch)
                continue;
            const size_t ichWord = ich - cch;
            bool match = ichWord == 0 || !std::isalnum(static_cast<unsigned char>(text[ichWord - 1]));
            for (size_t i = 0; match && i < cch; ++i)
                match = std::tolower(static_cast<unsigned char>(text[ichWord + i])) == s_achzWords[iword][i];
            if (match)
                text[ich + 2] = s_achStick[iword];
        }
    }
}

std::string PromptLabel(JOY_BUTTON button)
{
    if (button < 0 || button >= BTN_MAX)
        return "Unbound";

    if (PromptKeyboardActive())
        return KeyLabel(g_keyboardBindings[button]);

    if (!PromptSubstituteActive())
    {
        // The original layout names the PlayStation button of the action.
        static constexpr const char* s_apchzPs[BTN_MAX] =
        {
            "Up", "Down", "Left", "Right", "Square", "Circle", "Cross", "Triangle",
            "Start", "Select", "L1", "R1", "L2", "R2", "L3", "R3"
        };
        return s_apchzPs[button];
    }

    return cane::input::buttonLabel(cane::input::fromGlfwBinding(g_gamepadBindings[button]), PromptFamily());
}

void DrawPromptStripFixture()
{
    CFontBrx* pfont = g_pfont;
    if (pfont == nullptr || !FFontLoaded(1))
        return;

    // Every font-1 glyph, including the autosave mask which never changes,
    // over a dark band so the icons read on any scene.
    static const char* const s_pchzStrip =
        "&2X&. &2O&. &2S&. &2T&. &2L&. &2R&. &2l&. &2r&. &2(&. &2)&. &2+&. "
        "&2^&. &2v&. &2<&. &2>&. &2!&. &2_&. &2a&.";

    const float scale = g_gl.uiScale;
    pfont->PushScaling(scale, scale);
    const float lineHeight = static_cast<float>(pfont->m_dyUnscaled) * pfont->m_ryScale;
    const float y = (static_cast<float>(g_gl.height) - lineHeight) * 0.5f;
    glBlotShader.Use();
    pfont->SetupDraw();
    BindBlotTexture(whiteTex);
    RenderGlyphQuad(0.0f, y - lineHeight * 0.5f, static_cast<float>(g_gl.width), lineHeight * 2.0f,
        0.0f, 0.0f, 1.0f, 1.0f, glm::vec4(0.0f, 0.0f, 0.0f, 0.6f));
    CTextBox box;
    box.SetPos(0.0f, y);
    box.SetSize(static_cast<float>(g_gl.width), lineHeight);
    glm::vec4 color(1.0f);
    box.SetTextColor(&color);
    box.SetHorizontalJust(JH_Center);
    box.SetVerticalJust(JV_Top);
    CRichText text(const_cast<char*>(s_pchzStrip), pfont);
    text.Draw(&box, nullptr);
    pfont->PopScaling();
}

void TestPromptGlyphs()
{
    const auto require = [](bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(std::string("Prompt glyph check failed: ") + message);
    };

    // Restores every global the checks touch.
    struct RESTORE
    {
        std::array<int, BTN_MAX> gamepad = g_gamepadBindings;
        std::array<int, BTN_MAX> keyboard = g_keyboardBindings;
        bool originalPs = g_fAlwaysOriginalPsLayout;
        int style = g_promptStyle;
        ~RESTORE()
        {
            g_gamepadBindings = gamepad;
            g_keyboardBindings = keyboard;
            g_fAlwaysOriginalPsLayout = originalPs;
            g_promptStyle = style;
            cane::input::tracker().force(std::nullopt);
            cane::input::tracker().forceFromEnv();
        }
    } restore;

    const std::array<int, BTN_MAX> defaults = restore.gamepad;
    static constexpr char s_achGlyphs[] = "XOSTLRlr()+^v<>!_ab";

    // A synthetic controller font: identity of font 1, a distinct width per
    // glyph, no texture (only measurement is exercised here).
    CFontBrx font;
    font.m_grffont = 1U << 1;
    font.m_dyUnscaled = 32;
    font.m_dxSpaceUnscaled = 9;
    font.m_rxScale = 1.0f;
    font.m_ryScale = 1.0f;
    CFontBrx plain = font;
    plain.m_grffont = 1U << 0;
    for (int i = 0; s_achGlyphs[i] != '\0'; ++i)
    {
        GLYFF glyph{};
        glyph.wch = s_achGlyphs[i];
        glyph.x = static_cast<int16_t>(i * 40);
        glyph.dx = static_cast<int16_t>(20 + i);
        font.m_aglyff[static_cast<uint16_t>(glyph.wch)] = glyph;
        plain.m_aglyff[static_cast<uint16_t>(glyph.wch)] = glyph;
    }

    // Default bindings: every family measures exactly the original glyph
    // (an icon replacing Start / Select is square); substitution never
    // relayouts text.
    g_promptStyle = 0;
    for (int family = 0; family < cane::input::kFamilyCount; ++family)
        for (bool originalPs : { false, true })
        {
            cane::input::tracker().force(static_cast<Family>(family));
            g_fAlwaysOriginalPsLayout = originalPs;
            for (int i = 0; s_achGlyphs[i] != '\0'; ++i)
            {
                const char ch = s_achGlyphs[i];
                const PROMPTGLYPH glyph = PromptGlyphFor(ch);
                const bool wideIcon = glyph.kind == PROMPTGLYPH::Icon && (ch == '!' || ch == '_');
                require(wideIcon || font.DxFromCh(ch) == plain.DxFromCh(ch), "default widths changed");
                require(glyph.kind != PROMPTGLYPH::Glyph, "default bindings substituted a different glyph");
            }
        }

    // The autosave mask and ordinary text are never substituted.
    g_fAlwaysOriginalPsLayout = false;
    cane::input::tracker().force(Family::Xbox);
    require(PromptGlyphFor('a').kind == PROMPTGLYPH::Original && PromptGlyphFor('b').kind == PROMPTGLYPH::Original,
        "autosave glyphs substituted");
    require(PromptGlyphFor('Q').kind == PROMPTGLYPH::Original, "non-prompt glyph substituted");
    require(PromptButtonForGlyph('(') == Button::LS && PromptButtonForGlyph(')') == Button::RS &&
        PromptButtonForGlyph('+') == Button::Dpad, "stick and d-pad glyphs");
    {
        std::string text = "The left &2)&. controls the sub. Use the Right &2(&. to fire.";
        MatchStickGlyphsToWords(text);
        require(text == "The left &2(&. controls the sub. Use the Right &2)&. to fire.", "stick glyphs follow the words");
        g_fAlwaysOriginalPsLayout = true;
        text = "The left &2)&. controls the sub.";
        MatchStickGlyphsToWords(text);
        require(text == "The left &2)&. controls the sub.", "PlayStation icons keep the original text");
        g_fAlwaysOriginalPsLayout = false;
    }

    // Cross bound to the east button.
    g_gamepadBindings = defaults;
    g_gamepadBindings[BTN_CROSS] = GLFW_GAMEPAD_BUTTON_B;
    cane::input::tracker().force(Family::PlayStation);
    PROMPTGLYPH glyph = PromptGlyphFor('X');
    require(glyph.kind == PROMPTGLYPH::Glyph && glyph.ch == 'O', "PlayStation draws the bound Circle glyph");
    require(font.DxFromCh('X') == plain.DxFromCh('O'), "remapped glyph measures as the bound glyph");
    require(PromptLabel(BTN_CROSS) == "Circle", "PlayStation label of the remapped action");
    cane::input::tracker().force(Family::Xbox);
    require(PromptButtonForGlyph('X') == Button::East, "Xbox resolves Cross to the east button");
    require(PromptLabel(BTN_CROSS) == "B", "Xbox label of the remapped action");
    glyph = PromptGlyphFor('X');
    require(glyph.kind == PROMPTGLYPH::Icon || glyph.ch == 'O', "Xbox draws the east icon or its glyph");
    g_fAlwaysOriginalPsLayout = true;
    require(PromptFamily() == Family::PlayStation, "Original PlayStation Icons wins over the device");
    glyph = PromptGlyphFor('X');
    require(glyph.kind == PROMPTGLYPH::Original && glyph.ch == 'X', "Original PlayStation Icons is the original");
    require(font.DxFromCh('X') == plain.DxFromCh('X'), "PlayStation icons keep the original widths");
    require(PromptLabel(BTN_CROSS) == "Cross", "PlayStation labels");
    g_fAlwaysOriginalPsLayout = false;

    // Unbound actions and the Guide button keep the original glyph.
    g_gamepadBindings[BTN_CROSS] = -1;
    require(PromptGlyphFor('X').kind == PROMPTGLYPH::Original, "unbound action substituted");
    g_gamepadBindings[BTN_CROSS] = GLFW_GAMEPAD_BUTTON_GUIDE;
    require(PromptGlyphFor('X').kind == PROMPTGLYPH::Original, "Guide binding substituted");
    g_gamepadBindings = defaults;

    // Labels per family; Nintendo labels are positional.
    cane::input::tracker().force(Family::Nintendo);
    require(PromptLabel(BTN_CROSS) == "B" && PromptLabel(BTN_CIRCLE) == "A" && PromptLabel(BTN_R2) == "ZR",
        "Nintendo labels");
    cane::input::tracker().force(Family::SteamDeck);
    require(PromptLabel(BTN_START) == "Menu" && PromptLabel(BTN_SELECT) == "View", "Steam Deck labels");
    g_promptStyle = static_cast<int>(cane::input::PromptStyle::SteamController);
    require(PromptFamily() == Family::SteamController, "explicit prompt style wins over the device");
    g_promptStyle = 0;

    // Keyboard: the key's name, and (with the atlas) its keycap in the
    // original glyph's cell.
    cane::input::tracker().force(Family::Keyboard);
    g_keyboardBindings[BTN_CROSS] = GLFW_KEY_ENTER;
    require(PromptLabel(BTN_CROSS) == "Enter" && PromptKeyboardActive(), "keyboard labels name the key");
    glyph = PromptGlyphFor('X');
    require(glyph.ch == 'X' && (glyph.kind == PROMPTGLYPH::Icon || glyph.kind == PROMPTGLYPH::Original),
        "keyboard keycap keeps the original cell");
    require(PromptGlyphFor('(').kind == PROMPTGLYPH::Original, "keyboard keeps the stick glyphs");
    g_fAlwaysOriginalPsLayout = true;
    require(PromptGlyphFor('X').kind == PROMPTGLYPH::Original, "PlayStation icons win over the keyboard");
    g_fAlwaysOriginalPsLayout = false;

    // The Controller Mapping names keep the neutral keyboard labels.
    require(std::string(PchzGamepadBindingName(GLFW_GAMEPAD_BUTTON_A, Family::Keyboard)) == "A / Cross" &&
        std::string(PchzGamepadBindingName(GAMEPAD_BINDING_RIGHT_TRIGGER, Family::Keyboard)) == "Right Trigger" &&
        std::string(PchzGamepadBindingName(-1, Family::Keyboard)) == "Unbound" &&
        std::string(PchzGamepadBindingName(GLFW_GAMEPAD_BUTTON_B, Family::Nintendo)) == "A",
        "controller mapping names");
}
