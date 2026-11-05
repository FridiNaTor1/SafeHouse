// See promptglyph.h. After ThiefRush-Dev's Sly1-Release prompt_glyphs.cpp (PromptGlyphFor, PromptLabel); the
// keyboard keycaps are new (ThiefRush keeps the PlayStation glyphs for keyboard players).

#include "promptglyph.h"

#include "pcinput.h"

#include "../platform/button_prompts.h"
#include "../platform/system_settings.h"

#include <GLFW/glfw3.h>

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
}  // namespace

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
        int key = g_pcinput.keyboard[size_t(a)];
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
