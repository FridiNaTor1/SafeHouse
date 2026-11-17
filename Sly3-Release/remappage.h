#pragma once

// The Remap page (PC, 2026-11-10): one roomy screen over the pause menu for every binding, after Job Help's look (the
// title band, the column band, big rows with a grey selection bar and the selected row's text in yellow, the bottom
// band): a row per action with its keyboard / mouse binding (the keycap) and its controller binding (the button's
// icon for the controller in use, controller_family.h), the Advanced Input Mapping switch, Reset to Defaults, Back.
//
// It edits the launcher's stores: the standard bindings (g_pcinput: keyboard, gamepad, camera keys, tank keys) are
// saved to <user-dir>/Settings.ini (FSaveDisplaySettings, platform/sly2_settings.h); with Advanced Input Mapping on,
// the action bindings (sly2actions::profile) to the advanced controls file (actioninput.h Save). As the old pages:
// a standard key taken from another action swaps with it (BindKey's rule; a controller button too); advanced bindings
// may be shared. A standard controller binding cannot be empty (the settings' rule): Clear restores its default.
//
// Pad: up / down rows (with repeat), left / right the column, Cross change (the next key / button binds), Circle (the
// menus' erase button) clear, Triangle / Start back. Keyboard: the menu keys (arrows, Enter, Escape). Mouse: hover
// selects the row and column, a click changes, the wheel scrolls, the right button goes back.
// Opened from Controls and Prompts... (menu.cpp, its first row). (tests) SLY2N_REMAPPAGE=1 opens it with the menu.

bool FRemapPageOpen();
void OpenRemapPage();
void CloseRemapPage();
// The page's input, each frame the menu is shown (menu.cpp UpdateMenu: the menu takes no input while it is open).
void UpdateRemapPage();
// Drawn over everything (game.cpp, before the debug overlay).
void DrawRemapOverlay(int width, int height);
