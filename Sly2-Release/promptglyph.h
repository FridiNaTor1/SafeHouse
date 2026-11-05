#pragma once

// Dynamic controller prompts (Phase 3, after ThiefRush-Dev's Sly1-Release prompt_glyphs.h). The game's button
// icons are the glyphs of the HUD font's second set (the "icon font", text "&2X&."): X O S T Cross / Circle /
// Square / Triangle, L R l r L1 / R1 / L2 / R2, ! Start, _ Select, ( ) the sticks, + the d-pad. At draw time each
// one is resolved to the game action it names, then to the bound physical button (the gamepad bindings), then
// drawn for the device in use: the game's own icon of the bound button for PlayStation, a Kenney Input Prompts
// icon (CC0, licenses/kenney-input-prompts) for Xbox / Nintendo / Steam Deck / Steam Controller, the bound key's
// keycap for the keyboard. "Original PlayStation Icons" turns every substitution off. Glyph widths never
// change, so the game's layout stays the same in every mode.

#include "../platform/controller_family.h"

#include <string>

struct PROMPTGLYPH
{
    enum Kind { Original, Glyph, Icon };
    Kind kind = Original;
    char ch = '\0';              // Glyph: the game's icon to draw instead
    unsigned tex = 0;            // Icon: atlas texture and UVs (v0 = bottom edge, as the atlas is uploaded)
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

// The family the prompts are drawn for (Original PlayStation Icons, then the prompt style, then the last used
// device).
cane::input::Family PromptFamily();
// The keyboard (or mouse) was used last, whatever the style options say.
bool PromptKeyboardActive();
// How to draw icon-font glyph ch now.
PROMPTGLYPH PromptGlyphFor(char ch);
// The icon of the bound Circle (skipcut.cpp's indicator): the same resolution as PromptGlyphFor('O').
inline PROMPTGLYPH PromptGlyphCircle() { return PromptGlyphFor('O'); }
// The short name of the button bound to the action an icon glyph names ("B", "Circle", "E").
std::string PromptLabelFor(char ch);

// QoL (N39): the game's text names the Start and Select buttons by word ("Press START button for New Game", in the
// European languages "Touche START :", "START-Taste", ...). With the prompts dynamic (not "Original PlayStation
// Icons"), each whole upper-case word START / SELECT becomes the icon font's Start / Select glyph ("&2!&." /
// "&2_&."), which PromptGlyphFor then draws for the device in use. Text without those words, and every text with
// the option on, comes back unchanged. Only for text drawn with an icon font.
std::string StrPromptButtonWords(const std::string& text);

// QoL (N40): an icon-font glyph drawn as a fixed keyboard key's keycap (a GLFW key) while the keyboard is in use,
// for the PC's own hints that name a key (the debug menu's F10). Scoped: the override ends with the object.
struct PROMPTKEYGLYPH
{
    PROMPTKEYGLYPH(char ch, int glfwKey);
    ~PROMPTKEYGLYPH();
    PROMPTKEYGLYPH(const PROMPTKEYGLYPH&) = delete;
    PROMPTKEYGLYPH& operator=(const PROMPTKEYGLYPH&) = delete;
    char chPrev;
    int keyPrev;
};
