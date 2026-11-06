#pragma once

// The port's own UI text (the PC QoL: the pause menu's Exit to Desktop, the display / controls / keyboard options,
// the hold-to-skip hint, Replay Jobs) in the game's language (g_nLang): the European discs' 11 languages and the
// Japanese disc's Japanese. The USA disc (English) shows exactly the English strings. The debug menu stays English.
//
// Where the game has the word itself, the table uses the game's (its menus' On / Off, the Binocucom's name, the
// "Operation:" style and the word for a job in each language: porttext.cpp marks each row G (the game's words) or
// P (ours)); the game's own strings that exist as such ("Back") are read from the world's text instead.
// Strings are returned in the game's text encoding: Latin-1 on the USA / European discs (their fonts' 224 glyphs),
// the port's UTF-8 form of the 16-bit characters on the Japanese disc (region.h ChDecode). The Japanese strings use
// only the Japanese font's 1028 glyphs (kana, ASCII and its kanji).

#include <string>

enum PTX : int
{
    // separators
    PTX_Sep,            // a label and its value ("Music: On")
    PTX_GlyphSep,       // a button glyph and its action ("(X): Start Episode"; Japanese "(O)ボタン:")
    PTX_ListFirst, PTX_ListSep, PTX_ListEnd,   // a list in a sentence (" a, b, c.")
    PTX_On, PTX_Off,
    // the display options
    PTX_AspectRatio, PTX_FitToScreen, PTX_Resolution, PTX_Native, PTX_FrameRate, PTX_WindowMode, PTX_Windowed,
    PTX_Borderless, PTX_Fullscreen, PTX_VSync, PTX_DrawDistance,
    // the pause menu, the options' pages
    PTX_ExitToDesktop, PTX_DisplayOptions, PTX_ControlsAndPrompts,
    // the controls options
    PTX_ButtonIcons, PTX_Auto, PTX_OriginalPsIcons, PTX_InstantSkip, PTX_MouseCamera, PTX_MouseSensitivity,
    PTX_MouseAimSensitivity, PTX_WheelZoomsBinocucom, PTX_MouseInMenus,
    PTX_MouseAimInHacking, PTX_KeyboardMapping, PTX_ResetKeyboardMapping,
    // the keyboard mapping's rows
    PTX_MoveForward, PTX_MoveBack, PTX_MoveLeft, PTX_MoveRight, PTX_Square, PTX_Circle, PTX_Cross, PTX_Triangle,
    PTX_Start, PTX_Select, PTX_CameraLeft, PTX_CameraRight, PTX_CameraUp, PTX_CameraDown, PTX_PressAKey,
    // key and mouse names
    PTX_KeyUnbound, PTX_KeySpace, PTX_KeyUp, PTX_KeyDown, PTX_KeyLeft, PTX_KeyRight, PTX_KeyLeftMod, PTX_KeyRightMod,
    PTX_MouseLeft, PTX_MouseRight, PTX_MouseMiddle, PTX_MouseN, PTX_WheelUp, PTX_WheelDown, PTX_Mouse,
    // the cutscene skip hint
    PTX_HoldToSkip, PTX_HoldToSkipNoIcons,
    // Replay Jobs (the episode menu)
    PTX_ReplayJobs, PTX_ReplayJob, PTX_OpHippoDrop, PTX_ReplayOperation,
    // the save / load pages' page indicator (Q4, saves-qol: "Page %d / %d", the page and the pages)
    PTX_SlotPage,
    // the display options' frame-rate limit: none (R19)
    PTX_Uncapped,
    // the display options' Long Shadows (settings v16, render.cpp FBuildShadow2)
    PTX_LongShadows,
    // Invert Controls... (settings v17): its page entry, the controller's camera (with the right stick's icon), the
    // mouse in each view (the controller's binocucom / turret rows are the game's own options 0x51 / 0x53)
    PTX_InvertControlsPage, PTX_PadInvertCameraX, PTX_PadInvertCameraY, PTX_MouseInvertCameraX, PTX_MouseInvertCameraY,
    PTX_MouseInvertBinocX, PTX_MouseInvertBinocY, PTX_MouseInvertTurretX, PTX_MouseInvertTurretY,
    PTX_AutoCameraRotation,
    PTX_Max
};

// The string in the current language, in the game's text encoding.
std::string StrPort(PTX ptx);
// "<label><sep><value>" ("Mouse Camera: On"); a value from the port's table or any text already encoded.
std::string StrPortValue(PTX ptxLabel, const std::string& value);
std::string StrPortOnOff(PTX ptxLabel, bool f);
// A number with the language's decimal mark ("1.50" / "1,50").
std::string StrPortDecimal(float g, int cdigits);
// A UTF-8 string (a key's name from the system) in the game's text encoding.
std::string StrToGameText(const std::string& utf8);
// The key's or mouse button's name (pcinput.h PchzKeyName) in the current language where the table has it.
std::string StrPortKeyName(int key);
// The language is the USA disc's English (the strings are the port's original ones).
bool FPortEnglish();

// The game's own PS2 messages said for a PC (the memory card's: the save data, "do not close the game"; the USB
// headset option's "(for PlayStation 2)") in the current language, as UTF-8, or null when the string is not one of
// them. The disc's text id. Used where the game's strings are looked up (splayer.cpp StrLocalized) while
// FPcWording(): the "Original PlayStation Icons" option shows the original wording.
const char* SzPcWording(int idDisc);
bool FPcWording();
