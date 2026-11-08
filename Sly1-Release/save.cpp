#include "save.h"
#include "clock.h"
#include "cm.h"
#include "gl.h"
#include "gui_layout.h"
#include "glob.h"
#include "debug.h"
#include "sound.h"
#include "mouseinput.h"
#include "shd.h"
#include "Input.h"
#include "content.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <cstdlib>
#include "../platform/platform.h"

namespace
{
constexpr char kSaveMagic[8] = { 'P', 'C', 'A', 'N', 'E', 'S', 'A', 'V' };
constexpr uint32_t kSaveVersion = 3;
int s_lastSaveSlot = -1;
// The settings file as last loaded or written: the fields this build does not
// use (the v9 / v11 options of other builds and the launcher) are written back
// unchanged.
VIDEOSAVESETTINGS s_settingsFile = cane::settings::defaults();
// A newer build wrote the settings file: it is never overwritten.
bool s_fSettingsNewer = false;

// platform/system_settings.h repeats these engine values to stay GL-free.
static_assert(cane::settings::kBindingCount == BTN_MAX);
static_assert(cane::settings::kSaveSlotCount == SAVE_SLOT_COUNT);
static_assert(cane::settings::kKeyUnknown == GLFW_KEY_UNKNOWN && cane::settings::kKeySpace == GLFW_KEY_SPACE);
static_assert(cane::settings::kKeyEscape == GLFW_KEY_ESCAPE && cane::settings::kKeyLast == GLFW_KEY_LAST);
static_assert(cane::settings::kGamepadButtonLast == GLFW_GAMEPAD_BUTTON_LAST);
static_assert(cane::settings::kGamepadLeftTrigger == GAMEPAD_BINDING_LEFT_TRIGGER);
static_assert(cane::settings::kGamepadRightTrigger == GAMEPAD_BINDING_RIGHT_TRIGGER);
static_assert(static_cast<int>(cane::settings::Window_Fullscreen) == static_cast<int>(WindowMode_Fullscreen));
static_assert(static_cast<int>(cane::settings::Aspect_PS2_16_9) == static_cast<int>(PS2_16_9));
static_assert(static_cast<int>(cane::settings::Gui_Modern) == static_cast<int>(GuiStyle_Modern));
} // namespace

bool g_fAlwaysOriginalPsLayout = false;
int g_promptStyle = cane::settings::Prompt_Auto;
bool g_fAutoCameraRotation = true;
bool g_fInstantSkipCutscenes = false;
bool g_fDebugKeys = false;
bool g_fControllerVibration = true;

namespace
{

struct SaveFileHeader
{
    char magic[8];
    uint32_t version;
    uint32_t payloadSize;
    uint32_t checksum;
};

struct SavePayloadV2
{
    GS gameState;
    cane::settings::legacy::V1 video;
};

uint32_t SaveChecksum(const void* data, size_t size)
{
    return cane::settings::checksum(data, size);
}

VIDEOSAVESETTINGS CaptureVideoSettings()
{
    VIDEOSAVESETTINGS settings = s_settingsFile;
    settings.fogType = g_fogType;
    settings.msaaEnabled = g_fMsaa ? 1 : 0;
    settings.msaaSamples = g_msaaSamples;
    settings.frameRate = g_targetFrameRate;
    settings.internalResolutionHeight = g_internalResolutionHeight;
    settings.drawDistance = g_drawDistanceMultiplier;
    settings.windowMode = static_cast<int>(g_windowMode);
    settings.vsyncEnabled = g_fVsync ? 1 : 0;
    settings.aspectMode = static_cast<int>(g_gl.aspectMode);
    settings.guiScale = g_guiScale;
	settings.guiStyle = static_cast<int>(g_guiStyle);
	std::copy(g_keyboardBindings.begin(), g_keyboardBindings.end(), settings.keyboardBindings);
	std::copy(g_gamepadBindings.begin(), g_gamepadBindings.end(), settings.gamepadBindings);
    settings.disableControllerInputWhenUnfocused = g_fDisableControllerInputWhenUnfocused ? 1 : 0;
    settings.lastSaveSlot = s_lastSaveSlot;
	settings.stereoEnabled = FUserStereoEnabled() ? 1 : 0;
	settings.musicVolume = GetUserMusicVolume();
	settings.soundEffectsVolume = GetUserSfxVolume();
	settings.dialogueVolume = GetUserDialogueVolume();
	settings.alwaysOriginalPsPrompts = g_fAlwaysOriginalPsLayout ? 1 : 0;
	settings.promptStyle = g_promptStyle;
	settings.mouseCamera = g_mouseopt.fCamera ? 1 : 0;
	settings.autoCameraRotation = g_fAutoCameraRotation ? 1 : 0;
	settings.mouseSensitivity = g_mouseopt.sensitivity;
	settings.mouseAimSensitivity = g_mouseopt.aimSensitivity;
	settings.mouseInvertX = g_mouseopt.fInvertX ? 1 : 0;
	settings.mouseInvertY = g_mouseopt.fInvertY ? 1 : 0;
	settings.mouseRawInput = g_mouseopt.fRawInput ? 1 : 0;
	settings.mouseWheelZoom = g_mouseopt.fWheelZoom ? 1 : 0;
	settings.mouseMenus = g_mouseopt.fMenus ? 1 : 0;
	std::copy(g_cameraKeyBindings.begin(), g_cameraKeyBindings.end(), settings.cameraKeyBindings);
	settings.instantSkipCutscenes = g_fInstantSkipCutscenes ? 1 : 0;
    return settings;
}

GLFWmonitor* CurrentMonitor()
{
    if (g_gl.window != nullptr)
    {
        if (GLFWmonitor* monitor = glfwGetWindowMonitor(g_gl.window))
            return monitor;
    }

	int count = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&count);
	if (monitors != nullptr && count > 0)
		return monitors[std::clamp(g_monitorIndex, 0, count - 1)];
    return glfwGetPrimaryMonitor();
}

void ValidateVideoSettings(VIDEOSAVESETTINGS& settings)
{
    // The device-free rules are shared with the launcher (platform/system_settings.h).
    GLFWmonitor* monitor = CurrentMonitor();
    const GLFWvidmode* mode = monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
    GLint maxSamples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    cane::settings::sanitizeStatic(settings, mode != nullptr ? mode->height : 0,
        (std::max)(static_cast<int>(maxSamples), 1));
}

void ApplySavedVideoSettings(VIDEOSAVESETTINGS settings)
{
    ValidateVideoSettings(settings);

	if (!ApplyWindowModeSettings(static_cast<WindowMode>(settings.windowMode)))
		ApplyWindowModeSettings(WindowMode_Windowed);
    g_targetFrameRate = settings.frameRate;
    g_internalResolutionHeight = settings.internalResolutionHeight;
    g_fVsync = settings.vsyncEnabled != 0;
    g_fMsaa = settings.msaaEnabled != 0;
    g_msaaSamples = settings.msaaSamples;
    g_fogType = settings.fogType;
    g_drawDistanceMultiplier = settings.drawDistance;
    g_guiScale = settings.guiScale;
	g_guiStyle = static_cast<GuiStyle>(settings.guiStyle);
	std::copy(std::begin(settings.keyboardBindings), std::end(settings.keyboardBindings),
		g_keyboardBindings.begin());
	std::copy(std::begin(settings.gamepadBindings), std::end(settings.gamepadBindings),
		g_gamepadBindings.begin());
    g_fDisableControllerInputWhenUnfocused =
        settings.disableControllerInputWhenUnfocused != 0;
	// The shared settings' anisotropy (v11) drives upstream's per-texture filter.
	ApplyAnisotropicFilteringSettings(settings.anisotropy);
    s_lastSaveSlot = settings.lastSaveSlot;
	SetUserStereoEnabled(settings.stereoEnabled != 0);
	SetUserMusicVolume(settings.musicVolume);
	SetUserSfxVolume(settings.soundEffectsVolume);
	SetUserDialogueVolume(settings.dialogueVolume);
	g_fAlwaysOriginalPsLayout = settings.alwaysOriginalPsPrompts != 0;
	g_promptStyle = settings.promptStyle;
	g_mouseopt.fCamera = settings.mouseCamera != 0;
	g_fAutoCameraRotation = settings.autoCameraRotation != 0;
	g_mouseopt.sensitivity = settings.mouseSensitivity;
	g_mouseopt.aimSensitivity = settings.mouseAimSensitivity;
	g_mouseopt.fInvertX = settings.mouseInvertX != 0;
	g_mouseopt.fInvertY = settings.mouseInvertY != 0;
	g_mouseopt.fRawInput = settings.mouseRawInput != 0;
	g_mouseopt.fWheelZoom = settings.mouseWheelZoom != 0;
	g_mouseopt.fMenus = settings.mouseMenus != 0;
	std::copy(std::begin(settings.cameraKeyBindings), std::end(settings.cameraKeyBindings), g_cameraKeyBindings.begin());
	g_fInstantSkipCutscenes = settings.instantSkipCutscenes != 0;
	// (g_fDebugKeys: Debug mode, main.cpp LoadDebugModeSetting; the v14 debugKeys is ignored and kept as is)
	g_fControllerVibration = settings.controllerVibration != 0;
	s_settingsFile = settings;
	ApplyMouseOptions();

	ApplyAspectRatioSettings(static_cast<AspectMode>(settings.aspectMode));
	if (!ApplyInternalResolutionSettings())
	{
		g_internalResolutionHeight = 0;
		ApplyInternalResolutionSettings();
	}
    if (g_gl.renderHeight != g_internalResolutionHeight &&
        g_internalResolutionHeight != 0)
    {
        // A saved supersampling mode may be valid on one GPU but too expensive
        // on another. Keep the working native-sized framebuffer in that case.
        g_internalResolutionHeight = 0;
        ApplyInternalResolutionSettings();
    }
    ApplyMsaaSettings();
    ApplySwapInterval();

    glGlobShader.Use();
    glUniform1i(glslFogType, g_fogType);

    if (g_pcm != nullptr)
        SetCmMrdRatio(g_pcm, settings.drawDistance);
}

// GL_RENDERER tokens of Braswell, Bay Trail, Apollo Lake and Gemini Lake
// iGPUs, plus Mesa's software rasterizers. A token must not be followed by
// another digit, so "HD Graphics 400" does not match HD Graphics 4000.
bool FLowEndRenderer()
{
    if (const char* preset = std::getenv("CANE_PERF_PRESET"))
    {
        if (std::strcmp(preset, "low") == 0)
            return true;
        if (std::strcmp(preset, "default") == 0)
            return false;
    }

    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (renderer == nullptr)
        return false;

    constexpr std::string_view tokens[] = {
        "HD Graphics 400", "HD Graphics 405", "HD Graphics 500", "HD Graphics 505",
        "UHD Graphics 600", "UHD Graphics 605", "(BSW)", "(BYT)", "(APL)", "(GLK)",
        "llvmpipe", "softpipe" };
    const std::string_view name(renderer);

    for (std::string_view token : tokens)
    {
        for (size_t at = name.find(token); at != std::string_view::npos; at = name.find(token, at + 1))
        {
            const size_t end = at + token.size();
            if (end == name.size() || name[end] < '0' || name[end] > '9')
                return true;
        }
    }

    return false;
}

std::filesystem::path SaveDirectory()
{
    return cane::userDirectory() / "Saves";
}

std::filesystem::path SaveSlotPath(int slot)
{
    return SaveDirectory() / ("slot" + std::to_string(slot + 1) + ".sav");
}

std::filesystem::path SystemSettingsPath()
{
    return SaveDirectory() / "system_settings.dat";
}

bool WriteSaveSlot(int slot, const GS& save)
{
    std::error_code ec;
    std::filesystem::create_directories(SaveDirectory(), ec);
    if (ec)
        return false;

    const std::filesystem::path path = SaveSlotPath(slot);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    const GS payload = save;
    const SaveFileHeader header{ { 'P', 'C', 'A', 'N', 'E', 'S', 'A', 'V' }, kSaveVersion,
        static_cast<uint32_t>(sizeof(payload)), SaveChecksum(&payload, sizeof(payload)) };

    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream)
        return false;

    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    stream.write(reinterpret_cast<const char*>(&payload), sizeof(payload));
    stream.flush();
    if (!stream)
        return false;
    stream.close();

    return cane::atomicReplace(temporary, path);
}

bool ReadSaveSlot(int slot, GS* save, VIDEOSAVESETTINGS* legacyVideo, bool* hasLegacyVideo)
{
    if (save == nullptr || legacyVideo == nullptr || hasLegacyVideo == nullptr)
        return false;

    std::ifstream stream(SaveSlotPath(slot), std::ios::binary);
    SaveFileHeader header{};
    if (!stream.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
        std::memcmp(header.magic, kSaveMagic, sizeof(kSaveMagic)) != 0)
    {
        *save = {};
        *legacyVideo = {};
        *hasLegacyVideo = false;
        return false;
    }

    if (header.version == 1 && header.payloadSize == sizeof(GS))
    {
        GS loaded{};
        if (!stream.read(reinterpret_cast<char*>(&loaded), sizeof(loaded)) ||
            SaveChecksum(&loaded, sizeof(loaded)) != header.checksum)
            return false;

        *save = loaded;
        *legacyVideo = {};
        *hasLegacyVideo = false;
        return true;
    }

    if (header.version == 2 && header.payloadSize == sizeof(SavePayloadV2))
    {
        SavePayloadV2 payload{};
        if (!stream.read(reinterpret_cast<char*>(&payload), sizeof(payload)) ||
            SaveChecksum(&payload, sizeof(payload)) != header.checksum)
            return false;

        *save = payload.gameState;
        *legacyVideo = cane::settings::upgrade(payload.video);
        *hasLegacyVideo = true;
        return true;
    }

    if (header.version == kSaveVersion && header.payloadSize == sizeof(GS))
    {
        GS loaded{};
        if (!stream.read(reinterpret_cast<char*>(&loaded), sizeof(loaded)) ||
            SaveChecksum(&loaded, sizeof(loaded)) != header.checksum)
            return false;

        *save = loaded;
        *legacyVideo = {};
        *hasLegacyVideo = false;
        return true;
    }

    *save = {};
    *legacyVideo = {};
    *hasLegacyVideo = false;
    return false;
}

bool LoadSystemSettings(VIDEOSAVESETTINGS* settings)
{
    if (settings == nullptr)
        return false;

    const cane::settings::LoadResult result = cane::settings::load(SystemSettingsPath(), *settings);
    if (result == cane::settings::LoadResult::Newer)
        s_fSettingsNewer = true;
    return cane::settings::loaded(result);
}
}

bool SaveSystemSettings()
{
    // A file written by a newer build keeps its options (and is not downgraded).
    if (s_fSettingsNewer)
        return false;
    const VIDEOSAVESETTINGS settings = CaptureVideoSettings();
    if (!cane::settings::save(SystemSettingsPath(), settings))
        return false;
    s_settingsFile = settings;
    return true;
}

void StartupSaveBlot(SAVEBLOT* psaveblot)
{
    g_autosave.pvtsaveblot = &g_vtsaveblot;
}

void PostAutoSaveLoad(SAVEBLOT* psaveblot)
{
    PostBlotLoad(psaveblot);

    SetBlotDtAppear(psaveblot, 0.25f);
    SetBlotDtDisappear(psaveblot, 0.25f);
    SetBlotDtVisible(psaveblot, 0.0f);

    psaveblot->pfont = PfontFromFont(1);
    SetBlotFontScale(psaveblot, 0.69999999);

    char achzPrompt[2] =
    {
        GetAnimatedPromptCharacter(),
        '\0'
    };

    SetBlotAchzDraw(psaveblot, achzPrompt);

    // Keep the size calculated for the glyph, but initially draw no text.
    psaveblot->achzDraw[0] = '\0';
}

void SetSaveBlots(SAVEBLOT* psaveblot, BLOTS blots)
{
    if (blots == BLOTS_Hidden) 
        psaveblot->fSaveComplete = 0;

    SetBlotBlots(psaveblot, blots);
}

void DrawAutoSave(SAVEBLOT* psaveblot)
{
    if (g_fShowAutosaveIcon && psaveblot->blots == BLOTS_Hidden)
    {
        psaveblot->fSaveComplete = 0;
        ShowBlot(psaveblot);
    }

    if (psaveblot->blots == BLOTS_Hidden)
        return;

    const float dtVisible = g_clock.tReal - psaveblot->tBlots;

    if (!g_fShowAutosaveIcon &&
        psaveblot->blots == BLOTS_Visible && g_letterbox.blots == BLOTS_Visible)
        psaveblot->fSaveComplete = dtVisible < 2.0f;

    float yOffset = 0.0f;

    if (psaveblot->fSaveComplete)
        yOffset = (1.0f - g_letterbox.uOn) * 66.40001f;
    else if (!g_fShowAutosaveIcon &&
        psaveblot->blots == BLOTS_Visible && dtVisible > 1.0f)
        HideBlot(psaveblot);

    const GuiScale guiScale = GetGuiScale();
    const float dxAutosave = psaveblot->dx * guiScale.x;
    const float dyAutosave = psaveblot->dy * guiScale.y;

    // Retail positions this bottom-anchored blot after measuring its scaled
    // text. ProjectCane keeps the authored blot dimensions, so preserve the
    // already-correct 18/20 pixel margins and move the enlarged box upward by
    // the extra height instead of allowing it to grow into the letterbox.
    const float xAutosave = psaveblot->x;
    const float yAutosave = psaveblot->y - (dyAutosave - psaveblot->dy) +
        yOffset * guiScale.y;

    CTextBox tbx;
    tbx.SetPos(xAutosave, yAutosave);
    tbx.SetSize(dxAutosave, dyAutosave);
    tbx.SetTextColor(&psaveblot->rgba);
    tbx.SetHorizontalJust(JH_Left);
    tbx.SetVerticalJust(JV_Top);

    const char chPrompt = GetAnimatedPromptCharacter();
    const int percent = g_pgsCur != nullptr
        ? CalculatePercentCompletion(g_pgsCur)
        : 0;

    char achz[32];
    // Exact retail format: switch to the percentage font, apply its authored
    // 0.7 rich-text scale and yellow color, then restore the base font.
    std::snprintf(achz, sizeof(achz), "%c&1^07~b2a83d%d%%&.", chPrompt, percent);

    psaveblot->pfont->PushScaling(psaveblot->rFontScale, psaveblot->rFontScale);

    CRichText richText(achz, psaveblot->pfont);
    richText.Draw(&tbx, nullptr, guiScale.x, guiScale.y);

    psaveblot->pfont->PopScaling();
}

void AutosaveCurrentGame(SAVEDATA* psaveData)
{
    if (!SaveCurrentGameToDisk(psaveData))
        return;

    g_autosave.fSaveComplete = 0;
    if (g_autosave.pvtblot != nullptr && g_autosave.pvtblot->pfnShowBlot != nullptr)
        g_autosave.pvtblot->pfnShowBlot(&g_autosave);
}

void StartupSaveData(SAVEDATA* psaveData)
{
    if (psaveData == nullptr)
        return;

    *psaveData = {};

    VIDEOSAVESETTINGS settings{};
    bool hasSystemSettings = LoadSystemSettings(&settings);

    for (int slot = 0; slot < SAVE_SLOT_COUNT; ++slot)
    {
        VIDEOSAVESETTINGS legacySettings{};
        bool hasLegacySettings = false;
        ReadSaveSlot(slot, &psaveData->saveData[slot], &legacySettings, &hasLegacySettings);

        // Migrate the first old per-slot video block when no global settings
        // file exists yet. New game saves never carry system options.
        if (!hasSystemSettings && hasLegacySettings)
        {
            settings = legacySettings;
            hasSystemSettings = true;
        }
    }

    if (hasSystemSettings)
    {
        ApplySavedVideoSettings(settings);
        SaveSystemSettings();
    }
	else
	{
		settings = CaptureVideoSettings();
		cane::settings::defaultAudio(settings);
		// First run only; existing settings are never changed. The frame
		// rate defaults to 60 independently of the monitor's current mode.
		if (FLowEndRenderer())
		{
			settings.internalResolutionHeight = 480;
			std::cout << "Low-end graphics preset: internal resolution 480p" << std::endl;
		}
		ApplySavedVideoSettings(settings);
		SaveSystemSettings();
	}

    if (s_lastSaveSlot >= 0 && s_lastSaveSlot < SAVE_SLOT_COUNT &&
        psaveData->saveData[s_lastSaveSlot].dt > 0.0f)
    {
        psaveData->pgsAttractSave = &psaveData->saveData[s_lastSaveSlot];
    }

    for (GS& save : psaveData->saveData)
    {
        if (psaveData->pgsAttractSave == nullptr && save.dt > 0.0f)
        {
            psaveData->pgsAttractSave = &save;
            break;
        }
    }
}

bool SaveCurrentGameToDisk(SAVEDATA* psaveData)
{
    if (psaveData == nullptr || psaveData->pgsCurrentSave == nullptr || g_pgsCur == nullptr)
        return false;

    const ptrdiff_t slot = psaveData->pgsCurrentSave - psaveData->saveData;
    if (slot < 0 || slot >= SAVE_SLOT_COUNT)
        return false;

    GS snapshot = *g_pgsCur;
    // Existing slot menus use dt == 0 to mean "empty". Preserve that
    // convention while still allowing a brand-new game to be saved before
    // its first gameplay tick.
    if (snapshot.dt <= 0.0f)
        snapshot.dt = 0.000001f;
    snapshot.cbThis = sizeof(GS);
    snapshot.nChecksum = sizeof(GS);

    if (!WriteSaveSlot(static_cast<int>(slot), snapshot))
        return false;

    psaveData->saveData[slot] = snapshot;
    psaveData->pgsCurrentSave = &psaveData->saveData[slot];
    psaveData->pgsAttractSave = psaveData->pgsCurrentSave;
    s_lastSaveSlot = static_cast<int>(slot);
    SaveSystemSettings();
    return true;
}

bool DeleteSaveSlotFromDisk(SAVEDATA* psaveData, int slot)
{
    if (psaveData == nullptr || slot < 0 || slot >= SAVE_SLOT_COUNT)
        return false;

    std::error_code ec;
    const std::filesystem::path path = SaveSlotPath(slot);
    std::filesystem::remove(path, ec);

    // A missing file already represents an empty slot, so only an actual
    // filesystem error should make the erase operation fail.
    if (ec)
        return false;

    GS* erasedSave = &psaveData->saveData[slot];
    if (psaveData->pgsCurrentSave == erasedSave)
        psaveData->pgsCurrentSave = nullptr;
    if (psaveData->pgsAttractSave == erasedSave)
        psaveData->pgsAttractSave = nullptr;
    if (psaveData->pgsSelectedSave == erasedSave)
        psaveData->pgsSelectedSave = nullptr;

    *erasedSave = {};

    if (psaveData->pgsAttractSave == nullptr)
    {
        for (GS& save : psaveData->saveData)
        {
            if (save.dt > 0.0f)
            {
                psaveData->pgsAttractSave = &save;
                break;
            }
        }
    }

    return true;
}

void SetSaveManagerState(SAVEDATA* psavedata, SAVEMENUSTATE state)
{
    if (state != psavedata->state) {
        // Keep the chosen PC slot attached after the menu closes so level
        // completion, vault, clue, and coin autosaves continue writing to it.
        psavedata->state = state;
        psavedata->stateStartTime = g_clock.tReal;
    }
}

int LoadCurrentSave(SAVEDATA* psaveData)
{
    if (psaveData == nullptr || psaveData->pgsCurrentSave == nullptr || g_pgsCur == nullptr)
        return false;

    const GS* src = psaveData->pgsCurrentSave;
    if (src->dt <= 0.0f || src->gameWorldCur < GAMEWORLD_Intro ||
        src->gameWorldCur >= GAMEWORLD_Max || src->worldLevelCur < 0 ||
        src->worldLevelCur >= 9)
        return false;

    // GS is entirely persistent value data; it contains no runtime pointers.
    *g_pgsCur = *src;
    ApplyVibrationSetting(g_pgsCur);
    const ptrdiff_t slot = psaveData->pgsCurrentSave - psaveData->saveData;
    if (slot >= 0 && slot < SAVE_SLOT_COUNT)
    {
        psaveData->pgsAttractSave = psaveData->pgsCurrentSave;
        s_lastSaveSlot = static_cast<int>(slot);
        SaveSystemSettings();
    }
    return 1;
}

SAVEDATA g_saveData;
SAVEBLOT g_autosave;
VTSAVEBLOT g_vtsaveblot;

// Exercise the actual serializer before graphics/audio startup. Fixtures are
// synthetic little-endian files with the established Windows ABI.
void TestSaveCompatibility(const std::filesystem::path& fixtures)
{
    static_assert(sizeof(GS)==6656 && alignof(GS)==4 && sizeof(WS)==1100 && sizeof(LS)==120);
    static_assert(offsetof(GS,aws)==16 && offsetof(GS,gameWorldCur)==6616);
    static_assert(sizeof(SaveFileHeader)==20 && sizeof(cane::settings::Header)==20);
    static_assert(sizeof(cane::settings::legacy::V8)==196 && sizeof(VIDEOSAVESETTINGS)==340);
    static_assert(offsetof(VIDEOSAVESETTINGS,hackMouseAim)==288 && offsetof(VIDEOSAVESETTINGS,debugKeys)==292 && offsetof(VIDEOSAVESETTINGS,controllerVibration)==296 && offsetof(VIDEOSAVESETTINGS,longShadows)==300);
    static_assert(offsetof(VIDEOSAVESETTINGS,padInvertCameraX)==304 && offsetof(VIDEOSAVESETTINGS,mouseInvertTurretY)==324);
    static_assert(offsetof(VIDEOSAVESETTINGS,alwaysOriginalPsPrompts)==200 && offsetof(VIDEOSAVESETTINGS,instantSkipCutscenes)==284);
    const auto previous=cane::userOverride;
    const auto root=std::filesystem::temp_directory_path()/cane::path("cane save caf\xc3\xa9 "+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cane::userOverride=root;
    auto require=[](bool valid,const char* reason){if(!valid)throw std::runtime_error(reason);};
    auto bytes=[](const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);return std::vector<char>(std::istreambuf_iterator<char>(file),{});};
    try {
        std::filesystem::create_directories(SaveDirectory());
        GS expected{};
        for(int version=1;version<=3;++version) {
            std::filesystem::copy_file(fixtures/("slot-v"+std::to_string(version)+".sav"),SaveSlotPath(0),std::filesystem::copy_options::overwrite_existing);
            GS state{};VIDEOSAVESETTINGS video{};bool legacy=false;
            require(ReadSaveSlot(0,&state,&video,&legacy),"Legacy save fixture rejected");
            require(state.clife==7 && state.gameWorldCur==1 && state.worldLevelCur==1 && state.grfgs==0x100 && state.unlockedCutscenes==5,"Save fields changed layout");
            require(legacy==(version==2),"Legacy embedded settings lost");expected=state;
        }
        require(WriteSaveSlot(0,expected),"Save writer failed with Unicode path");
        require(bytes(SaveSlotPath(0))==bytes(fixtures/"slot-v3.sav"),"Save bytes differ from Windows fixture");
        // The shared header's defaults must match the engine's built-in values.
        {const VIDEOSAVESETTINGS builtIn=CaptureVideoSettings(),shared=cane::settings::defaults();
         require(std::memcmp(&builtIn,&shared,sizeof(shared))==0,"Shared settings defaults differ from the engine");}
        for(int version=1;version<=8;++version) {
            std::filesystem::copy_file(fixtures/("settings-v"+std::to_string(version)+".dat"),SystemSettingsPath(),std::filesystem::copy_options::overwrite_existing);
            VIDEOSAVESETTINGS settings{};
            require(LoadSystemSettings(&settings),"Settings fixture rejected");
            require(settings.frameRate==120 && settings.internalResolutionHeight==720,"Settings fields changed layout");
            require(settings.musicVolume==(version==8?.5f:1.0f),"Audio settings upgrade failed");
            require(settings.alwaysOriginalPsPrompts==0 && settings.promptStyle==0 && settings.mouseCamera==1 &&
                    settings.cameraKeyBindings[0]==GLFW_KEY_LEFT && settings.instantSkipCutscenes==0,"New fields lack their defaults");
        }
        // A v14 file (before controllerVibration) loads with vibration on.
        {VIDEOSAVESETTINGS file=cane::settings::defaults();file.debugKeys=1;file.controllerVibration=0;
         require(cane::settings::save(SystemSettingsPath(),file,14),"v14 settings writer failed");
         VIDEOSAVESETTINGS loaded{};uint32_t version=0;
         require(cane::settings::load(SystemSettingsPath(),loaded,&version)==cane::settings::LoadResult::Upgraded && version==14 &&
                 loaded.debugKeys==1 && loaded.controllerVibration==1,"v14 settings upgrade failed");}
        // A v15 file (before longShadows, Sly 2's) loads with long shadows on.
        {VIDEOSAVESETTINGS file=cane::settings::defaults();file.controllerVibration=0;file.longShadows=0;
         require(cane::settings::save(SystemSettingsPath(),file,15),"v15 settings writer failed");
         VIDEOSAVESETTINGS loaded{};uint32_t version=0;
         require(cane::settings::load(SystemSettingsPath(),loaded,&version)==cane::settings::LoadResult::Upgraded && version==15 &&
                 loaded.controllerVibration==0 && loaded.longShadows==1,"v15 settings upgrade failed");}
        // A v16 file (before Sly 2's v17 inversion fields) loads with them off and its own fields, the mouse's v11
        // inversion included, kept.
        {VIDEOSAVESETTINGS file=cane::settings::defaults();file.longShadows=0;file.mouseInvertX=1;file.mouseInvertY=1;
         file.padInvertCameraX=file.padInvertCameraY=file.mouseInvertBinocX=file.mouseInvertBinocY=file.mouseInvertTurretX=file.mouseInvertTurretY=1;
         require(cane::settings::save(SystemSettingsPath(),file,16),"v16 settings writer failed");
         VIDEOSAVESETTINGS loaded{};uint32_t version=0;
         require(cane::settings::load(SystemSettingsPath(),loaded,&version)==cane::settings::LoadResult::Upgraded && version==16 &&
                 loaded.longShadows==0 && loaded.mouseInvertX==1 && loaded.mouseInvertY==1 && loaded.padInvertCameraX==0 &&
                 loaded.padInvertCameraY==0 && loaded.mouseInvertBinocX==0 && loaded.mouseInvertBinocY==0 &&
                 loaded.mouseInvertTurretX==0 && loaded.mouseInvertTurretY==0,"v16 settings upgrade failed");}
        // Older settings retain the original automatic camera behavior.
        {VIDEOSAVESETTINGS file=cane::settings::defaults();file.autoCameraRotation=0;file.mouseSensitivity=170;
         require(cane::settings::save(SystemSettingsPath(),file,17),"v17 settings writer failed");
         VIDEOSAVESETTINGS loaded{};uint32_t version=0;
         require(cane::settings::load(SystemSettingsPath(),loaded,&version)==cane::settings::LoadResult::Upgraded && version==17 &&
                 loaded.autoCameraRotation==1 && loaded.mouseSensitivity==170,"v17 camera settings upgrade failed");}
        // Round-trip the current writer (v18), with the fields Sly 1 does not use kept as loaded.
        {VIDEOSAVESETTINGS file=cane::settings::defaults();file.fovPercent=35;file.showFps=1;file.streamerMode=1;file.hackMouseAim=0;file.debugKeys=1;file.controllerVibration=0;file.longShadows=0;
         file.padInvertCameraX=1;file.padInvertCameraY=1;file.mouseInvertBinocX=1;file.mouseInvertBinocY=1;file.mouseInvertTurretX=1;file.mouseInvertTurretY=1;
         require(cane::settings::save(SystemSettingsPath(),file),"Settings writer failed");
         VIDEOSAVESETTINGS loaded{};require(LoadSystemSettings(&loaded),"Current settings rejected");
         s_settingsFile=loaded;}
        const bool originalPs=g_fAlwaysOriginalPsLayout,instant=g_fInstantSkipCutscenes,autoCamera=g_fAutoCameraRotation;const int promptStyle=g_promptStyle;
        const MOUSEOPTIONS mouse=g_mouseopt;const auto cameraKeys=g_cameraKeyBindings;
        g_fAlwaysOriginalPsLayout=true;g_promptStyle=cane::settings::Prompt_SteamController;g_fInstantSkipCutscenes=true;
        g_fAutoCameraRotation=false;
        g_mouseopt.aimSensitivity=320;g_mouseopt.fInvertY=true;g_cameraKeyBindings[3]=GLFW_KEY_KP_2;
        const bool written=SaveSystemSettings();
        g_fAlwaysOriginalPsLayout=originalPs;g_promptStyle=promptStyle;g_fInstantSkipCutscenes=instant;
        g_fAutoCameraRotation=autoCamera;
        g_mouseopt=mouse;g_cameraKeyBindings=cameraKeys;
        VIDEOSAVESETTINGS current{};uint32_t version=0;
        require(written && cane::settings::load(SystemSettingsPath(),current,&version)==cane::settings::LoadResult::Ok &&
                version==cane::settings::kVersion,"Settings not written at the current version");
        require(current.alwaysOriginalPsPrompts==1 && current.promptStyle==cane::settings::Prompt_SteamController &&
                current.instantSkipCutscenes==1,"Prompt settings lost");
        require(current.mouseAimSensitivity==320 && current.mouseInvertY==1 && current.cameraKeyBindings[3]==GLFW_KEY_KP_2,"Mouse settings lost");
        require(current.autoCameraRotation==0,"Camera rotation setting lost");
        require(current.fovPercent==35 && current.showFps==1 && current.streamerMode==1 && current.hackMouseAim==0 && current.debugKeys==1 && current.controllerVibration==0 && current.longShadows==0 &&
                current.padInvertCameraX==1 && current.padInvertCameraY==1 && current.mouseInvertBinocX==1 && current.mouseInvertBinocY==1 &&
                current.mouseInvertTurretX==1 && current.mouseInvertTurretY==1,"Other builds' settings not kept");
        // A file from a newer build is reported, never accepted or overwritten.
        {auto newer=bytes(SystemSettingsPath());newer[8]=static_cast<char>(cane::settings::kVersion+1);
         std::ofstream output(SystemSettingsPath(),std::ios::binary|std::ios::trunc);output.write(newer.data(),newer.size());}
        require(!LoadSystemSettings(&current),"Newer settings version accepted");
        const auto newerBytes=bytes(SystemSettingsPath());
        require(!SaveSystemSettings() && bytes(SystemSettingsPath())==newerBytes,"Newer settings overwritten");
        s_fSettingsNewer=false;s_settingsFile=cane::settings::defaults();
        std::ofstream(SaveSlotPath(0),std::ios::binary|std::ios::trunc)<<"truncated";
        GS state{};VIDEOSAVESETTINGS settings{};bool legacy=false;
        require(!ReadSaveSlot(0,&state,&settings,&legacy),"Truncated save accepted");
        auto damaged=bytes(fixtures/"slot-v3.sav");damaged.back()^=1;
        {std::ofstream output(SaveSlotPath(0),std::ios::binary);output.write(damaged.data(),damaged.size());}
        require(!ReadSaveSlot(0,&state,&settings,&legacy),"Save checksum corruption accepted");
        std::filesystem::remove_all(root);cane::userOverride=previous;
    }catch(...){std::filesystem::remove_all(root);cane::userOverride=previous;throw;}
}
