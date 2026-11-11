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
#include <string_view>

namespace cane::sly2input { enum class Action; }

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
    PTX_MouseAimSensitivity, PTX_MouseWheelZoom, PTX_MouseInMenus,
    PTX_MouseAimInHacking, PTX_KeyboardMapping, PTX_ResetKeyboardMapping,
    // the keyboard mapping's rows
    PTX_MoveForward, PTX_MoveBack, PTX_MoveLeft, PTX_MoveRight, PTX_Square, PTX_Circle, PTX_Cross, PTX_Triangle,
    PTX_Start, PTX_Select, PTX_CameraLeft, PTX_CameraRight, PTX_CameraUp, PTX_CameraDown, PTX_TankLeftTrack, PTX_TankRightTrack, PTX_PressAKey,
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
    PTX_AdvancedBindings, PTX_ActionBindings, PTX_ActionKeyboard, PTX_ActionController,
    PTX_ResetAdvancedBindings, PTX_CaptureAction,
    // the Remap page (remappage.h): its entry, title, column heads, last rows, hints and capture lines
    PTX_RemapPage, PTX_RemapTitle, PTX_RemapAction, PTX_RemapKeyboard, PTX_RemapController, PTX_RemapReset,
    PTX_RemapBack, PTX_RemapChange, PTX_RemapClear, PTX_RemapPressKey, PTX_RemapPressButton, PTX_RemapFixed,
    // (VR, vr.h) the pause menu's VR Settings page (shown with a headset only)
    PTX_VrSettings, PTX_VrMode, PTX_VrImmersive, PTX_VrFlatScreen, PTX_VrVignette, PTX_VrLow, PTX_VrMedium, PTX_VrHigh,
    PTX_VrScreenSize, PTX_VrSmall, PTX_VrLarge, PTX_VrRefreshRate, PTX_VrEyeResolution, PTX_VrFoveation, PTX_VrMultiview, PTX_VrUpscaling, PTX_VrSharpen, PTX_VrSuperResolution, PTX_VrDynamic, PTX_VrWorldScale, PTX_VrRecenter,
    PTX_VrCutscenes, PTX_VrRcTable,
    // the pause menu's Exit to Desktop when the game was started from SafeHouse's Collection Style Launcher
    // (SAFEHOUSE_LAUNCHER=collection: quitting returns to it)
    PTX_QuitToLauncher,
    // (audio, notes/AUDIO_2026-11-10.md) the options' Audio Devices page: the game's output, the microphone (USB
    // Headset), the headset's speaker; a device's values ("%s (not connected)": a chosen device that is missing)
    PTX_AudioDevicesPage, PTX_OutputDevice, PTX_InputDevice, PTX_HeadsetOutputDevice, PTX_SystemDefault, PTX_SameAsOutput,
    PTX_NotConnected,
    // (langstore.h) the language menu's entry of a language whose text this disc cannot show: "Japanese (voices only)"
    PTX_LanguageVoicesOnly,
    // (the options' Language... page, region.h g_nLangVoice) its entry, its two rows, the voices' default, a choice
    // that takes effect when the next world loads
    PTX_LanguagePage, PTX_TextLanguage, PTX_VoiceLanguage, PTX_SameAsText, PTX_FromNextArea,
    PTX_Max
};

// The string in the current language, in the game's text encoding.
std::string StrPort(PTX ptx);
// Started from SafeHouse's Collection Style Launcher (SAFEHOUSE_LAUNCHER=collection): the pause menu's Exit to Desktop
// says Quit to Launcher (PTX_QuitToLauncher; quitting returns to that launcher, which waits for the game).
bool FQuitToLauncher();
// "<label><sep><value>" ("Mouse Camera: On"); a value from the port's table or any text already encoded.
std::string StrPortValue(PTX ptxLabel, const std::string& value);
std::string StrPortOnOff(PTX ptxLabel, bool f);
// A number with the language's decimal mark ("1.50" / "1,50").
std::string StrPortDecimal(float g, int cdigits);
// A UTF-8 string (a key's name from the system) in the game's text encoding.
std::string StrToGameText(const std::string& utf8);
// The key's or mouse button's name (pcinput.h PchzKeyName) in the current language where the table has it.
std::string StrPortKeyName(int key);
std::string StrPortActionName(cane::sly2input::Action action);
std::string StrPortActionGroup(std::string_view group);
// The language is the USA disc's English (the strings are the port's original ones).
bool FPortEnglish();

// The game's own PS2 messages said for a PC (the memory card's: the save data, "do not close the game"; the USB
// headset option's "(for PlayStation 2)") in the current language, as UTF-8, or null when the string is not one of
// them. The disc's text id. Used where the game's strings are looked up (splayer.cpp StrLocalized) while
// FPcWording(): the "Original PlayStation Icons" option shows the original wording.
const char* SzPcWording(int idDisc);
bool FPcWording();

// The pause menu's "Exit to Episode Menu" in the languages whose text says "DVD menu" (Dutch, Portuguese, Danish,
// Finnish, Norwegian, Swedish) named as the others name it (UTF-8; null: the game's own string). splayer.cpp
// StrLocalized, always (not a PS2-for-PC wording: a naming fix), unless a text mod changed the string.
const char* SzEpisodeMenuWording(int idDisc);
