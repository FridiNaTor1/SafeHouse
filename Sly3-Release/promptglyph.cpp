// See promptglyph.h. After ThiefRush-Dev's Sly1-Release prompt_glyphs.cpp (PromptGlyphFor, PromptLabel); the
// keyboard keycaps are new (ThiefRush keeps the PlayStation glyphs for keyboard players).

#include "promptglyph.h"

#include "pcinput.h"

#include "../platform/button_prompts.h"
#include "../platform/system_settings.h"

#include <GLFW/glfw3.h>

#include <cctype>
#include <cstdint>

namespace
{
using cane::input::Button;
using cane::input::Family;

// The icon font's glyphs that name a game action.
struct GLYPHACTION { char ch; PCACTION action; };
constexpr GLYPHACTION s_aglyphaction[] = {
    {'X', PCA_Cross}, {'O', PCA_Circle}, {'S', PCA_Square}, {'T', PCA_Triangle},
    {'L', PCA_L1}, {'R', PCA_R1}, {'l', PCA_L2}, {'r', PCA_R2}, {'!', PCA_Start}, {'_', PCA_Select},
};

int ActionFromGlyph(char ch)
{
    for (const GLYPHACTION& g : s_aglyphaction)
        if (g.ch == ch) return g.action;
    return -1;
}

// The positional button an icon glyph means now (through the gamepad bindings), Button::Count when it is not a
// button glyph or the action is unbound. (The sticks and the d-pad are not remappable.)
Button ButtonForGlyph(char ch)
{
    switch (ch)
    {
    case '(': return Button::LS;
    case ')': return Button::RS;
    case '+': return Button::Dpad;
    default: break;
    }
    const int a = ActionFromGlyph(ch);
    if (a < 0) return Button::Count;
    return cane::input::fromGlfwBinding(g_pcinput.gamepad[size_t(a)]);
}

// PROMPTKEYGLYPH's override: this icon glyph is drawn as this key while the keyboard is in use
char s_chKeyOverride = '\0';
int s_keyOverride = 0;
}  // namespace

PROMPTKEYGLYPH::PROMPTKEYGLYPH(char ch, int glfwKey) : chPrev(s_chKeyOverride), keyPrev(s_keyOverride)
{
    s_chKeyOverride = ch;
    s_keyOverride = glfwKey;
}

PROMPTKEYGLYPH::~PROMPTKEYGLYPH()
{
    s_chKeyOverride = chPrev;
    s_keyOverride = keyPrev;
}

std::string StrPromptButtonWords(const std::string& text)
{
    if (g_pcinput.fAlwaysOriginalPs) return text;
    if (text.find("START") == std::string::npos && text.find("SELECT") == std::string::npos) return text;
    struct WORD { const char* sz; size_t cch; const char* szIcon; };
    static const WORD s_aword[] = {{"START", 5, "&2!&."}, {"SELECT", 6, "&2_&."}};
    auto fLetter = [](unsigned char ch) { return std::isalnum(ch) || ch >= 0x80; };
    std::string out;
    out.reserve(text.size());
    bool fIconFont = false;   // (inside an "&2 ... &." run the words are glyph letters, not text)
    for (size_t i = 0; i < text.size();)
    {
        if (text[i] == '&' && i + 1 < text.size())
        {
            if (text[i + 1] != '&') fIconFont = text[i + 1] != '.';
            out.append(text, i, 2);
            i += 2;
            continue;
        }
        bool fDone = false;
        if (!fIconFont && (i == 0 || !fLetter(uint8_t(text[i - 1]))))
            for (const WORD& w : s_aword)
                // (after the word: not another capital or digit; a lower-case letter is a compound's next part, as the
                // Swedish "STARTknappen")
                if (text.compare(i, w.cch, w.sz) == 0 &&
                    (i + w.cch == text.size() || !(std::isupper(uint8_t(text[i + w.cch])) || std::isdigit(uint8_t(text[i + w.cch])))))
                {
                    out += w.szIcon;
                    i += w.cch;
                    fDone = true;
                    break;
                }
        if (!fDone) out += text[i++];
    }
    return out;
}

Family PromptFamily()
{
    return cane::input::promptFamily(g_pcinput.promptStyle, g_pcinput.fAlwaysOriginalPs ? 1 : 0);
}

bool PromptKeyboardActive() { return cane::input::tracker().family() == Family::Keyboard; }

PROMPTGLYPH PromptGlyphFor(char ch)
{
    PROMPTGLYPH glyph;
    glyph.ch = ch;
    if (g_pcinput.fAlwaysOriginalPs) return glyph;
    const Family family = PromptFamily();
    const auto& atlas = cane::input::promptAtlas();

    // the keyboard: the bound key's keycap (keys the atlas has; else the game's icon)
    if (family == Family::Keyboard)
    {
        const int a = ActionFromGlyph(ch);
        if (a < 0) return glyph;
        int key = ch == s_chKeyOverride && s_chKeyOverride ? s_keyOverride : g_pcinput.keyboard[size_t(a)];
        if (a == PCA_Start && key == cane::settings::kKeyUnknown) key = GLFW_KEY_ESCAPE;
        const cane::input::IconUV uv = atlas.keyUV(cane::input::promptKeyForBinding(key));
        if (!uv.valid || !atlas.keyboardTexture()) return glyph;
        glyph.kind = PROMPTGLYPH::Icon;
        glyph.tex = atlas.keyboardTexture();
        glyph.u0 = uv.u0; glyph.v0 = uv.v0; glyph.u1 = uv.u1; glyph.v1 = uv.v1;
        return glyph;
    }

    const Button button = ButtonForGlyph(ch);
    if (button == Button::Count) return glyph;
    // the game's icon of the button actually bound (remapped controls shown honestly)
    const char chBound = cane::input::psGlyphFor(button);
    if (chBound != '\0')
    {
        glyph.kind = chBound == ch ? PROMPTGLYPH::Original : PROMPTGLYPH::Glyph;
        glyph.ch = chBound;
    }
    if (family == Family::PlayStation) return glyph;
    const unsigned tex = atlas.texture(family);
    const cane::input::IconUV uv = atlas.uv(family, button);
    if (!tex || !uv.valid) return glyph;
    glyph.kind = PROMPTGLYPH::Icon;
    glyph.tex = tex;
    glyph.u0 = uv.u0; glyph.v0 = uv.v0; glyph.u1 = uv.u1; glyph.v1 = uv.v1;
    return glyph;
}

std::string PromptLabelFor(char ch)
{
    const int a = ActionFromGlyph(ch);
    if (a < 0) return "";
    if (PromptKeyboardActive() && !g_pcinput.fAlwaysOriginalPs) return PchzKeyName(g_pcinput.keyboard[size_t(a)]);
    if (g_pcinput.fAlwaysOriginalPs)
    {
        static const char* const s_apsz[PCA_Max] = {"Up", "Down", "Left", "Right", "Square", "Circle", "Cross", "Triangle",
                                                    "Start", "Select", "L1", "R1", "L2", "R2", "L3", "R3"};
        return s_apsz[a];
    }
    return cane::input::buttonLabel(cane::input::fromGlfwBinding(g_pcinput.gamepad[size_t(a)]), PromptFamily());
}
