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

#include <optional>
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
PROMPTGLYPH PromptGlyphForBinding(int binding, cane::input::Family family);

// Use the action described by an authored prompt, even when it shares an original button.
struct PROMPTTEXTGLYPHS
{
    explicit PROMPTTEXTGLYPHS(int textId);
    ~PROMPTTEXTGLYPHS();
    PROMPTTEXTGLYPHS(const PROMPTTEXTGLYPHS&) = delete;
    PROMPTTEXTGLYPHS& operator=(const PROMPTTEXTGLYPHS&) = delete;
private:
    int previous;
};
// The icon of the bound Circle (skipcut.cpp's indicator): the same resolution as PromptGlyphFor('O').
inline PROMPTGLYPH PromptGlyphCircle() { return PromptGlyphFor('O'); }
// The short name of the button bound to the action an icon glyph names ("B", "Circle", "E").
std::string PromptLabelFor(char ch);
std::string StrPromptFallbackText(const std::string& text);

// QoL (N39): the game's text names the Start and Select buttons by word ("Press START button for New Game", in the
// European languages "Touche START :", "START-Taste", ...). With the prompts dynamic (not "Original PlayStation
// Icons"), each whole upper-case word START / SELECT becomes the icon font's Start / Select glyph ("&2!&." /
// "&2_&."), which PromptGlyphFor then draws for the device in use. Text without those words, and every text with
// the option on, comes back unchanged. Only for text drawn with an icon font.
std::string StrPromptButtonWords(const std::string& text, int textId);

// (VR) The prompts' family for the VR controllers in use (platform/vr_pad_map.h Controllers, as int): Index ValveIndex,
// the Vive wands and WMR ViveWand (the Pads layout: each trackpad's click a face button), the Steam Frame SteamFrame, Pico
// Pico, the others (Touch, Vive Focus 3 / Cosmos, HP Reverb G2, the generic profile) MetaQuest.
cane::input::Family VrPromptFamily(int controllers);
// The VR controllers the prompts of a VR family show: those in use (vr::ControllersInUse) when that is their family,
// else the family's own (MetaQuest Touch, ValveIndex Index, SteamFrame Frame, ViveWand Vive, Pico Pico); 0 (None) for a
// family that is not a VR one. (The generic profile shows as Touch.)
int VrControllersFor(cane::input::Family family);
// A button's icon for a VR family where the controllers in use differ from the family's art: WMR's sticks (the Vive
// wand art has trackpads), the HP Reverb G2's Select (its left menu button; the Touch art's Select is the grip + stick
// click combo). Unchanged otherwise.
void VrButtonIcon(cane::input::Button& button, cane::input::Family& family);
// cane::input::buttonLabel with the same differences ("Left stick" on WMR, "Menu (Left)" on the HP Reverb G2).
std::string PromptButtonName(cane::input::Button button, cane::input::Family family);

// (VR) The icons of the Select ('_') / Start ('!') glyph as an icon run ("&2...&."), the grip + stick-click combo's two
// icons with "+" between where the controllers in use make them so (the Controls page's combo labels).
std::string StrPromptComboIcons(char ch);

// An icon-font glyph drawn as a fixed keyboard key (e.g. the debug menu's F10).
// Optionally override its controller binding for contextual action cues. Scoped to this object's lifetime.
struct PROMPTKEYGLYPH
{
    PROMPTKEYGLYPH(char ch, int glfwKey, int controllerBinding = -2,
                   std::optional<cane::input::Family> family = std::nullopt);
    ~PROMPTKEYGLYPH();
    PROMPTKEYGLYPH(const PROMPTKEYGLYPH&) = delete;
    PROMPTKEYGLYPH& operator=(const PROMPTKEYGLYPH&) = delete;
    char chPrev;
    int keyPrev;
    int padPrev;
    std::optional<cane::input::Family> familyPrev;
};
