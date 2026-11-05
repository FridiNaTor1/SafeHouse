#pragma once
// Dynamic controller prompts (from ThiefRush-Dev's Sly1-Release
// prompt_glyphs.h). Original prompts are font-1 ("joy" font) glyphs such as
// "&2X&." (Cross). At draw time each glyph is resolved to the game action it
// names, then to the bound physical button (or key), then drawn for the device
// in use: the original glyph of the bound button for PlayStation, a Kenney
// Input Prompts icon (CC0, licenses/kenney-input-prompts) for Xbox / Nintendo
// / Steam Deck / Steam Controller, and the bound key's keycap for keyboard
// players (as Sly2-Native does; ThiefRush keeps the PlayStation glyphs there).
// "Original PlayStation Icons" turns every substitution off. Glyph widths
// never change, so the game's layout is the same in every mode.
#include "Input.h"
#include "../platform/controller_family.h"
#include <string>

struct PROMPTGLYPH
{
    enum Kind { Original, Glyph, Icon };
    Kind kind = Original;
    char ch = '\0';              // Glyph: the original glyph to draw instead (Icon: the cell's glyph)
    GLuint texture = 0;          // Icon: atlas texture and font-quad UVs
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

// After glfwInit(): joystick identification, bundled mappings, test override
// (PROJECTCANE_TEST_PROMPT_FAMILY).
void StartupPromptInput();
// After the GL context exists (InitGL): uploads the icon atlases.
void StartupPromptAtlas();
// Before the GL context is destroyed (TerminateGL).
void ShutdownPromptAtlas();

// The family prompts are drawn for (Original PlayStation Icons, then the
// Button Icons setting, then the last used device).
cane::input::Family PromptFamily();
// Whether the keyboard (or mouse) was used last, whatever the settings say.
bool PromptKeyboardActive();
// False while "Original PlayStation Icons" is on.
bool PromptSubstituteActive();

// The game action a font-1 glyph stands for, or BTN_MAX ('+', '(', ')', the
// autosave mask 'a'/'b' and anything else).
JOY_BUTTON JoyButtonFromPromptGlyph(char ch);
// The positional button a font-1 glyph currently means (through the gamepad
// bindings), or Button::Count when it is not a button glyph or is unbound.
cane::input::Button PromptButtonForGlyph(char ch);
// How to draw font-1 glyph ch right now.
PROMPTGLYPH PromptGlyphFor(char ch);
// The icon of the bound Circle (the skip indicator, skipcut.cpp).
inline PROMPTGLYPH PromptGlyphCircle() { return PromptGlyphFor('O'); }
// Original level text names the stick in words but sometimes uses the other
// stick's glyph ("The left &2)&. controls the sub"). The device icons would
// then contradict the sentence, so the glyph after "left "/"right " is made to
// agree with the word. Only while icons are substituted (Always Show
// PlayStation Icons keeps the original text).
void MatchStickGlyphsToWords(std::string& text);

// Plain-text name of the key bound to an action for keyboard players,
// otherwise of the bound button for the current family ("A", "Cross", "ZR").
std::string PromptLabel(JOY_BUTTON button);

// Hidden test hook (--test-prompt-strip): one rich-text line with every
// font-1 glyph in the screen centre.
void DrawPromptStripFixture();

// --test-prompt-glyphs (also run by --test-save-compatibility): headless
// checks of the glyph mapping, widths and labels. Throws on failure.
void TestPromptGlyphs();
