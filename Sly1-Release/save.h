#pragma once
#include "blot.h"
#include "game.h"
#include "../platform/system_settings.h"

inline constexpr int SAVE_SLOT_COUNT = 6;

enum SAVEMENUSTATE
{
    SAVE_MENU_STATE_Nil = 0,
    SAVE_MENU_STATE_Selecting = 1,
    SAVE_MENU_STATE_Closing = 2
};

struct SAVEBLOT : public BLOT
{
    int fSaveComplete;
};

// Saves/system_settings.dat payload. The format and its serializer live in
// platform/system_settings.h, shared with the launcher's Settings page (and Sly2-Native).
using VIDEOSAVESETTINGS = cane::settings::SystemSettings;

struct SAVEDATA
{
    SAVEMENUSTATE state;
    float stateStartTime;
    GS saveData[SAVE_SLOT_COUNT];
    GS* pgsCurrentSave;
    GS* pgsAttractSave;
    GS* pgsSelectedSave;
};

void StartupSaveBlot(SAVEBLOT* psaveblot);
void PostAutoSaveLoad(SAVEBLOT* psaveblot);
void SetSaveBlots(SAVEBLOT* psaveblot, BLOTS blots);
void DrawAutoSave(SAVEBLOT* psaveblot);

void StartupSaveData(SAVEDATA* psaveData);
bool SaveSystemSettings();
// Applies g_fVsync and g_targetFrameRate to the swap interval. Call it after
// either changes and after a window-mode change.
void ApplySwapInterval();
bool SaveCurrentGameToDisk(SAVEDATA* psaveData);
bool DeleteSaveSlotFromDisk(SAVEDATA* psaveData, int slot);
void AutosaveCurrentGame(SAVEDATA* psaveData);
void SetSaveManagerState(SAVEDATA* psavedata, SAVEMENUSTATE state);
int  LoadCurrentSave(SAVEDATA* psaveData);

// Persisted with the system settings (v10). "Original PlayStation Icons":
// draw the original PlayStation glyphs whatever controller is in use.
extern bool g_fAlwaysOriginalPsLayout;
// Persisted with the system settings (v10). cane::input::PromptStyle value,
// 0 = Automatic (follow the last used device), clamped to 0-5.
extern int g_promptStyle;
// Persisted with the system settings (v12). One press of Circle skips a
// cutscene (else it is held for 3 s). Off by default.
extern bool g_fInstantSkipCutscenes;
// Debug mode (the launcher's Settings > Advanced > Debug mode, Debug Mode.ini; main.cpp LoadDebugModeSetting, off by
// default): only then does F3 (the debug window) do anything. The system settings' v14 debugKeys is ignored.
extern bool g_fDebugKeys;
// The system settings' v15 controllerVibration (the launcher's Settings > Controls > Controller vibration, on by
// default): off, the pad's motors are never driven (Input.cpp SetSdlRumble), whatever the game's own Vibration option.
extern bool g_fControllerVibration;

extern SAVEDATA g_saveData;
extern SAVEBLOT g_autosave;
extern bool g_fSwapPacesFrames;
extern unsigned g_swapGeneration;
