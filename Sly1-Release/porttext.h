#pragma once

// The PC port's own UI text in the game's language: the pause menu's Exit to Desktop, the Video / Sound / Controls
// / Mouse options and their one-line help, the keyboard and controller mapping, the key and mouse button names and
// the hold-to-skip hint. The languages are the installation's (content.h): the European disc's English, French,
// German, Spanish and Italian, the Japanese disc's Japanese, the Korean disc's Korean. The USA disc shows the
// English strings exactly as before. (Sly 2 has the same kind of table: Sly2-Release/porttext.h.)
//
// Where the game has the word itself the table uses the game's (porttext.cpp marks rows G: its On / Off, the
// Binocucom's name, the inverted-axis rows' wording, its separators); the game's own strings that exist as such
// ("New Game", "Empty", "Controls") are read from its localization table instead (content.h ContentLocalizedText).
// Strings are UTF-8. The Japanese strings use only the Japanese text font's glyphs (kana, ASCII and about 330
// kanji), the European ones only the PAL font's, the Korean ones Hangul (drawn from the font's jamo) and ASCII:
// tools/sly1porttextcheck.py checks them against the fonts.

#include <string>

enum PTX : int
{
    // separators, values
    PTX_Sep,            // a label and its value ("Music Volume: 80%")
    PTX_On, PTX_Off,
    // the pause and options menus' entries (G for the menus' "..." style)
    PTX_ExitToDesktop, PTX_VideoEntry, PTX_SoundEntry, PTX_KeyboardMappingEntry, PTX_ControllerMappingEntry,
    PTX_MouseEntry,
    // the menus' titles
    PTX_Video, PTX_Sound, PTX_KeyboardMapping, PTX_ControllerMapping, PTX_Mouse, PTX_AreYouSure,
    // Video
    PTX_Msaa, PTX_FrameRate, PTX_Fps, PTX_Performance, PTX_Resolution, PTX_NativeResolution, PTX_Fog, PTX_Ps2Style,
    PTX_Ps3Style, PTX_DrawDistance, PTX_Low, PTX_Original, PTX_High, PTX_VeryHigh, PTX_Extreme, PTX_Custom,
    PTX_WindowMode, PTX_Windowed, PTX_Borderless, PTX_Fullscreen, PTX_VSync, PTX_AspectRatio, PTX_FitToScreen,
    PTX_GuiScale, PTX_GuiStyle, PTX_Modern, PTX_DisableControllerUnfocused,
    // Sound
    PTX_MusicVolume, PTX_SfxVolume, PTX_DialogueVolume,
    // Controls (the camera row: the game's inverted-axis rows' wording)
    PTX_CameraTurnLeft, PTX_CameraTurnRight, PTX_ButtonIcons, PTX_Automatic, PTX_OriginalPsIcons, PTX_InstantSkip,
    // Mouse
    PTX_MouseCamera, PTX_CameraSensitivity, PTX_AimSensitivity, PTX_InvertHorizontal, PTX_InvertAimVertical,
    PTX_RawInput, PTX_WheelZoomsBinocucom, PTX_MouseInMenus,
    // the keyboard mapping's rows
    PTX_MoveUp, PTX_MoveDown, PTX_MoveLeft, PTX_MoveRight, PTX_Square, PTX_Circle, PTX_Cross, PTX_Triangle,
    PTX_Start, PTX_Select, PTX_CameraLeft, PTX_CameraRight, PTX_CameraUp, PTX_CameraDown, PTX_PressAKey,
    PTX_PressAButton,
    // key and mouse button names
    PTX_KeyUnbound, PTX_KeySpace, PTX_KeyEnter, PTX_KeyBackspace, PTX_KeyLeftShift, PTX_KeyRightShift,
    PTX_KeyLeftCtrl, PTX_KeyRightCtrl, PTX_KeyLeftAlt, PTX_KeyRightAlt, PTX_KeyUp, PTX_KeyDown, PTX_KeyLeft,
    PTX_KeyRight, PTX_Key, PTX_MouseLeft, PTX_MouseRight, PTX_MouseMiddle, PTX_MouseN, PTX_WheelUp, PTX_WheelDown,
    PTX_MouseButton,
    // the options' one-line help
    PTX_HelpPromptStyle, PTX_HelpOriginalPsIcons, PTX_HelpInstantSkip, PTX_HelpMouse, PTX_HelpMouseCamera,
    PTX_HelpMouseSensitivity, PTX_HelpAimSensitivity, PTX_HelpInvertHorizontal, PTX_HelpInvertAimVertical,
    PTX_HelpRawInput, PTX_HelpWheelZoom, PTX_HelpMouseMenus, PTX_HelpCameraKeys,
    // the cutscene skip hint
    PTX_HoldToSkip, PTX_HoldToSkipNoIcons,
    PTX_AutoCameraRotation, PTX_HelpAutoCameraRotation,
    PTX_Max
};

// The string in the current language.
const char* PchzPort(PTX ptx);
// "<label><sep><value>" ("Mouse Camera: On").
std::string StrPortValue(PTX ptxLabel, const std::string& value);
std::string StrPortOnOff(PTX ptxLabel, bool f);
// A printf format from the table, filled in ("%d FPS", "Custom (%.2fx)"); a decimal comma where the language
// writes one.
std::string StrPortFormat(PTX ptx, ...);
// The table's column: 0 English (also the USA disc), 1 French, 2 German, 3 Spanish, 4 Italian, 5 Japanese,
// 6 Korean.
int IlangPort();
