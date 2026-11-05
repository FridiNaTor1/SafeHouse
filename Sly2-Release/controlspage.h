#pragma once

// (QoL, Phase 3; not the original) The pause menu's "Controls" page for the device in use. The original page (menu
// screen object 0x370, one part per character: Sly / Bentley / Murray) is a PS2 controller drawn from vector sprites
// (element groups 20..27), one sprite of leader lines (a group-0 sprite placed at the canvas origin), the lines' end
// dots (group 28) and nine text labels, the actions named after the PS2 buttons.
//
// - PlayStation prompts with the default pad bindings, and "Original PlayStation Icons": the original, untouched.
// - PlayStation prompts with remapped pad bindings: the original picture, each leader line ending at the button its
//   action is bound to.
// - Xbox / Nintendo / Steam Deck / Steam Controller / PS4 / PS5: Kenney's picture of that controller (CC0,
//   prompts_devices.png) in the PS2 picture's place and the device's button icons, each label's line ending at the
//   button its action is bound to; "R1 / L3 / R3 button" in the labels named for the bound buttons.
// - Keyboard and mouse: a list of the same labels, each with the keycaps / mouse buttons bound to its action (the
//   live bindings).
// The title, the portrait, the background and the labels' font and colours stay the game's own. Drawn from
// CScreenPlayer::Draw (splayer.cpp). SLY2N_CTLPAGE=0 turns it off.

#include "hud.h"

#include <string>
#include <vector>

struct FONTBRX;
struct SCREENOBJ;

// One drawn piece of the screen object (a leaf of its group tree) with its draw state.
struct CTLPIECE
{
    int igroup = -1;
    const SELEM* pelem = nullptr;
    HUDSTATE st;
    float mx = 0, my = 0;        // the slot's position (canvas units)
};

// Before a screen object's pieces are drawn: whether it shows a Controls page that is drawn differently now (the
// pieces are then passed through FControlsPageSkip / FControlsPageText and ControlsPageEnd closes the draw).
struct SW;
bool FControlsPageBegin(const SCREENOBJ* pso, const std::vector<CTLPIECE>& apiece, const FONTBRX* pfont,
                        const FONTBRX* pfontIcons, const SW* psw);
// A piece about to be drawn: true when it is not drawn (the picture's pieces); the replacement is drawn in its place.
bool FControlsPageSkip(const CTLPIECE& piece);
// A text label about to be drawn (its id and text; the text may be changed): true when it is not drawn here (the
// keyboard list draws it at the end).
struct TEXTDRAW;
bool FControlsPageText(const TEXTDRAW& td, std::string& text);
// After the pieces: the keyboard list.
void ControlsPageEnd(const FONTBRX* pfont, const FONTBRX* pfontIcons);
