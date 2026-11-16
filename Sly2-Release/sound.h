#pragma once

// The game's sound module (Sly 2 0x1FAF70..0x1FF690, Sly 1's sound.c reworked around sound objects): the sound
// manager g_sound (0x3DD040: the world's sound bank, its sound / music / dialog tables from the world file, the
// volumes by channel and kind, the music stack, the dialog player), and the sound objects that talk to 989snd
// (snd989.h: banked effects) and spiop (spiop.h: streamed music, ambience and dialog).
//
// Interface for the world loader: LoadMusicFromBrx (the global block 0x1FD0E8, right after the world groups),
// ResetSoundMgr before a world loads (0x1FD700), LoadSoundBank after (0x1FD5E8; also each frame by
// UpdateSoundMgr). For the game: StartupSound once, UpdateSoundMgr each frame, PsndStartSound (effects by the
// world's sound id), PsndPushMusic / PopMusic (music by id), PsndStartDialog / StopDialog (dialog lines by id),
// the volume setters, and SNDHOOKS for what the sound module asks the object model.

#include "disc.h"
#include "basic.h"

#include <array>
#include <deque>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

class CBinaryInputStream;
class CRef;

// What the sound module needs from the game; null hooks are treated as "nothing there".
struct SNDHOOKS
{
    // The listener (the current/forced player, 0x1C4A48(g_game) / 0x1A62E8; without one the world camera at
    // 0x2DDF40 + 0x90) and its rotation world -> camera (the matrix at 0x2DDFA0, as 0x1A7348(0, mat, d) uses
    // it). False: no listener (sounds are heard at full volume, centered).
    bool (*pfnListener)(glm::vec3* ppos, glm::mat3* pmatWorldToCamera) = nullptr;
    // A sound source (an ALO: bit 60 of +0x28 its "alive" flag, 0x1A6F00 a local point to world).
    bool (*pfnFSourceAlive)(void* psrc) = nullptr;
    glm::vec3 (*pfnSourcePosWorld)(void* psrc, const glm::vec3& posLocal) = nullptr;
    // The game running (0x2BD4C0 == 3); otherwise (menus, loading) music and dialog pause. Null: running.
    bool (*pfnFGameRunning)() = nullptr;
    // The music stem the player's state selects (0x1FDC08: 0 sneaking / hiding (state 3, a target, jobs 0..2
    // and 13), 2 alerted (state 4), 1 otherwise; -1: no player, use 1).
    int (*pfnMusicStem)() = nullptr;
    // Whether a footstep sound's character is the current player (0x1FBC58: 0x6F Sly D_002E1E40, 0x73 Bentley
    // D_002DD5BC, 0x15 Murray D_002F7900 against 0x1C4840(g_game)): its start then steps the mood (0x1FEED0).
    bool (*pfnFMoodStep)(int sfxid) = nullptr;
    // The game's random number in [lo, hi] (0x11D3F8), for the mood's step.
    int (*pfnRandInRange)(int lo, int hi) = nullptr;
    // A dialog line started (0x227728: subtitles; idDialog, oidSpeaker) / its events (0x1FE0E8: target 0..5 of
    // the speaker (0x15ED78 + 0x1F67E0 with the ids at 0x2C3C40), value an int, a float for target 3).
    void (*pfnDialogStarted)(int idDialog, int oidSpeaker) = nullptr;
    void (*pfnDialogEvent)(int oidSpeaker, int itarget, int32_t value) = nullptr;
    // The line's targets (0x1FE5F0: a line with a speaker and events finds the speaker (0x15ED78 kind 5) and its
    // effect SMAs (0x1F67E0 with the SMs 0x1FD0C0 i: +0x22C..+0x240, target 3 the speaker itself) / at its end
    // (0x1FE738: target 5's goal 0x611, then released)
    void (*pfnDialogBind)(int oidSpeaker) = nullptr;
    void (*pfnDialogUnbind)() = nullptr;
    // The game clock's t (0x2DDED0: the subtitle's blinking cursor).
    float (*pfnTGame)() = nullptr;
    // The localized text of an id (0x13E368), for the subtitles.
    std::string (*pfnText)(int id) = nullptr;
    // The objects whose position the music's 3D stems (voices 3 and 4) follow (0x2DD5BC, 0x2F7900, 0x2E1E40).
    std::array<void*, 3> apsrcMusic{};
    // Bumped by ResetSoundMgr (a world change): the glue finds its characters again.
    int cReset = 0;
    // Finds apsrcMusic now (a music pushed by the new world's scripts before its first sound pass).
    void (*pfnFindMusicSources)() = nullptr;
    // (orig 0x1FDA48, the USB headset's microphone) the current player (0x1C4840(g_game)) when it is an actor (cid
    // 0x34), its world position (0x1A62E8); the noise held (0x156BE0: the old one removed and released; the template
    // the world's fixed 711 (SW +0x4034) with the radius / height range given, null: none). Null hooks: no noise.
    void* (*pfnMicPlayer)(glm::vec3* ppos) = nullptr;
    void (*pfnHoldMicNoise)(void** ppnoise, void* pplayer, const glm::vec3& pos, float sRadius, float dz) = nullptr;
};
extern SNDHOOKS g_sndhooks;

// ---------------------------------------------------------------------------------------------------------
// Sound objects

class CSnd;

// The original's Ref (0x259B48 assign / 0x259BB0 construct): a counted reference to a sound object.
class CSndRef
{
public:
    CSndRef() = default;
    CSndRef(CSnd* p);
    CSndRef(const CSndRef& o) : CSndRef(o.m_p) {}
    CSndRef& operator=(const CSndRef& o);
    ~CSndRef();
    CSnd* get() const { return m_p; }
    CSnd* operator->() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }

private:
    CSnd* m_p = nullptr;
};

// The sound object (vtable 0x2D9D00 base). The dirty mask +0x50 drives the per-frame update (0x1FB8E0):
// 1 start, 2 pause, 4 resume, 8 stop, 0x10 volume, 0x20 pan, 0x40 pitch, 0x80 / 0x100 voice volumes / pans,
// 0x200 3D (attached to a source or at a position), 0x400 poll "still playing", 0x800 the play clock runs,
// 0x1000 poll "started".
class CSnd : public BASIC
{
public:
    CSnd();
    virtual ~CSnd();
    CSnd(const CSnd&) = delete;
    CSnd& operator=(const CSnd&) = delete;
    void AddRef() { ++m_cref; }                    // slot 1
    void Release();                                // slot 2
    virtual void Start();                          // slot 7: 0x1FB0C0
    virtual int Kind() { return 1; }               // slot 8 (2: a looper / a looping stream)
    virtual void StartIop() {}                     // slot 9
    virtual void StopIop() {}                      // slot 10
    virtual void PauseIop() {}                     // slot 11
    virtual void ResumeIop() {}                    // slot 12
    virtual void SendChanges() {}                  // slot 13
    virtual void Requests() {}                     // slot 14
    virtual void OnAudible(int fInaudible) { (void)fInaudible; }  // slot 15
    virtual void Update3D();                       // slot 16: 0x1FB528

    void Pause();                                  // 0x1FB018
    void Resume();                                 // 0x1FB060
    void Stop(float tFade = 0.0f);                 // 0x1FB098
    void SetVolume(float g);                       // 0x1FB1F8
    void SetPan(int deg);                          // 0x1FB268
    void SetPitch(float g);                        // 0x1FB2C0
    void SetSource(void* psrc);                    // 0x1FB2F8
    void SetPosition(const glm::vec3& pos);        // 0x1FB3F0
    void SetFalloff(float sStart, float sEnd);     // 0x1FB4F0
    void SetDirty(uint32_t mask, uint32_t value);  // 0x1FB658
    void RemoveActive();                          // 0x1FB710
    void UpdateFrame();                            // 0x1FB8E0
    [[nodiscard]] bool FPlaying() const;           // playing or about to (+0x40, +0x52, start pending)

    float m_gVol = 1.0f;          // +0x10
    int m_pan = 0;                // +0x14
    float m_pitch = 0.0f;         // +0x18
    void* m_psrc = nullptr;       // +0x1C
    glm::vec3 m_pos{};            // +0x20
    float m_sStart = 300.0f;      // +0x30 (full volume within)
    float m_sEnd = 3000.0f;       // +0x34 (silent beyond)
    uint32_t m_h = 0;             // +0x40 the 989snd handle / spiop id
    float m_gVolCur = 1.0f;       // +0x44 with the 3D falloff
    float m_tFade = 0.0f;         // +0x48
    uint16_t m_cref = 0;          // +0x4C
    int16_t m_cpause = 0;         // +0x4E
    uint16_t m_grfDirty = 0;      // +0x50
    uint16_t m_cbusy = 0;         // +0x52 (waiting for the IOP side)
};

// The sound BASIC's instance API (class 0x97, EOPIDs 127..136).
CRef RefSoundObjectOption(CSnd* psnd, int bifk, int optid, const CRef* value = nullptr);
// A returned script sound owns a reference independently of active playback.
CRef RefRetainSound(CSnd* psnd);                 // 0x129A10 / 0x1294C0

// A banked sound effect (0x70 bytes, vtable 0x2D9C70): the world's sound id (+0x60) through g_sound's tables.
class CSndPlain : public CSnd
{
public:
    explicit CSndPlain(int sfxid);                 // 0x1FBBC8
    int Kind() override;                           // 0x1FBC20
    void StartIop() override;                      // 0x1FBC58
    void StopIop() override;                       // 0x1FBE80
    void PauseIop() override;                      // 0x1FBEE0
    void ResumeIop() override;                     // 0x1FBF00
    void SendChanges() override;                   // 0x1FBF20
    void Requests() override;                      // 0x1FBFB0
    void OnAudible(int fInaudible) override;       // 0x1FC018
    int m_sfxid = 0;                               // +0x60
    // (PC addition, pcloader.h: a kept world's sound, from its own bank: set by PsndNewPlainBank, or found by its
    // source's world through g_pfnPcSndBank)
    uint32_t m_hbankPc = 0;
    int m_isoundPc = -1, m_kindPc = 0;
    bool FResolve(uint32_t* phbank, int* pisound, int* pkind) const;
};

struct SNDSTREAM;

// A streamed sound (0x3C0 bytes, vtable 0x2D9BE0): a world file's music / ambience / dialog entry through
// spiop, with six channel-id voices (volume, pan, up to 8 falloff entries each).
class CSndVoice : public CSnd
{
public:
    CSndVoice(SNDSTREAM* pstream, uint32_t flags);  // 0x1FC190
    void Start() override;                          // 0x1FC270
    int Kind() override;                            // 0x1FC4F8
    void StartIop() override;                       // 0x1FC510
    void StopIop() override;                        // 0x1FC698
    void PauseIop() override;                       // 0x1FC6D0
    void ResumeIop() override;                      // 0x1FC708
    void SendChanges() override;                    // 0x1FC740
    void Requests() override;                       // 0x1FC878
    void OnAudible(int) override {}                 // 0x1FC9A8
    void Update3D() override;                       // 0x1FC9B0
    void SetVoiceVolume(int k, float g);            // 0x1FC2E0
    void SetVoicePan(int k, int deg);               // 0x1FC380
    void AddVoiceFalloff(int k, void* psrc, float sStart, float sEnd, float g);  // 0x1FC408

    struct FALLOFF
    {
        void* psrc = nullptr;
        float sStart = 0, sEnd = 0, g = 1;
    };
    SNDSTREAM* m_pstream = nullptr;                 // +0x60
    double m_tPlay = 0.0;                           // +0x68 (the original counts EE cycles)
    std::array<float, 6> m_agVoice{};               // +0x70
    std::array<int, 6> m_apanVoice{};               // +0x88
    std::array<std::vector<FALLOFF>, 6> m_aafalloff; // +0xA0 counts, +0xB8 entries
    uint32_t m_flags = 0;                           // +0x3B8 (4: loop)
    uint32_t m_grfVoiceChanged = 0;                 // +0x3BC
};

// ---------------------------------------------------------------------------------------------------------
// The world's tables (0x1FD0E8)

// A streamed file entry: a music track (0x48 bytes, listed at +0xF0), the resident ambience (+0xF4), or a
// dialog line (0x5C bytes, listed at +0xEC; the key of the current language).
struct SNDSTREAM
{
    uint8_t key[0x40]{};    // +0x00 the file key
    FK fk;
    int id = -1;            // +0x40
    // dialog lines
    int oidSpeaker = -1;    // +0x48
    int fNoHeadset = 0;     // +0x4C (s8: 0 or -1; the USB headset plays the line when 0)
    float tLength = 0.0f;   // +0x50
    struct EVENT
    {
        int16_t dt60;       // after the previous one, in 1/60 s
        int16_t itarget;    // 0..5 (-1 none)
        int32_t value;      // an int (a float for target 3)
    };
    std::vector<EVENT> aevent;  // +0x54 / +0x58
};

struct SOUNDMGR
{
    // the bank (0x1FD5E8)
    uint8_t keyBank[0x40]{};          // +0xA8
    uint32_t hbank = 0;               // +0xE8
    std::vector<int> aflagsSound;     // +0xFC / +0xF8 per bank sound: its kind (2 = a looper, once known)
    std::vector<int> aisoundFromId;   // +0x100 / +0x18 the world's sound id -> bank sound (-1 none)
    // streams
    std::deque<SNDSTREAM> adialog;    // +0xEC (file order; the original's list is newest first)
    std::deque<SNDSTREAM> amusic;     // +0xF0
    std::unique_ptr<SNDSTREAM> president;  // +0xF4
    CSndRef sndResident;              // +0x228
    std::vector<int> aidTarget;       // +0x1D4 / +0x1D0 (up to 6)
    std::vector<uint8_t> alanguage;   // the file's languages; ilanguage the one matching g_nLang (region.h)
    int ilanguage = 0;
    // state
    int fEffect = 0;                  // +0x10 the USB headset in use (flags "fUseHeadset", 0x1D8FF8; cheat row 10)
    int fMusicPaused = 0;             // +0x58
    int fModeAlert = 0, fModeSneak = 0;  // +0x5C / +0x60
    int istem = 1;                    // +0x64
    uint32_t grfState = 3;            // +0x68
    int playbackMode = 0;             // +0x104
    // (the USB headset's microphone, 0x1FDA48) +0x70 its level ('SUKR' 11: spiop.irx's RMS of the last 1024 samples,
    // 0..1), +0x74 the noise held at the player while it is above 0.5, +0x80 / +0x90 the noise's radius / height range
    // by the level (0x1FCD38: quadratics through (0, 0), (0.5, 500), (1, 3000) and (0, 0), (0.5, 500), (1, 1000))
    float gMic = 0.0f;
    void* pnoiseMic = nullptr;
    std::array<float, 3> clqMicRadius{0.0f, -1000.0f, 4000.0f};
    std::array<float, 3> clqMicHeight{0.0f, 1000.0f, 0.0f};
    std::array<float, 3> agStem{};    // +0x108
    std::array<float, 9> asmpStem{3.0f, 0.0f, 0.25f, 1.0f, 0.0f, 0.25f, 1.0f, 0.0f, 0.25f};  // +0x114
    int mood = 0;                     // +0x138
    std::array<std::array<float, 4>, 8> agVol{};  // +0x13C [kind][channel] (kind 2 channel 0, +0x15C, is the level)
    float gMaster = 1.0f;             // +0x1BC
    std::array<float, 4> agChannel{}; // +0x1C0
    double tClock = 0.0, tClockPrev = 0.0;  // +0x1F0 / +0x1F8 (raw ticks converted to seconds)
    CSndRef sndDialog;                // +0x200
    std::vector<CSndRef> asndMusic;   // +0x204 / +0x208 (8 at most; the top is the last)
    std::array<int, 6> aoidTarget{};  // +0x22C (the dialog's targets; here only their indices are passed on)
    int oidDialogSpeaker = -1;
    int ievent = 0;                   // +0x244
    float tEvent = 0.0f;              // +0x248
    float tDialog = 0.0f;             // +0x24C
    std::vector<CSnd*> apsndActive;   // +0x4C (DL)
};
extern SOUNDMGR g_sound;
extern SOUNDMGR* g_psoundKeptLoad;
// (PC addition, pcloader.h) a plain sound of another bank (a kept world's: its bank sound and kind, 2 a looper); and the
// hook that finds a source's bank (a copy of a kept world's object: its world's), false for the running world's
CSndPlain* PsndNewPlainBank(int sfxid, uint32_t hbank, int isound, int kind);
// (PC addition, pcloader.h) a sound a kept world's copy makes without a source (its explosion's, R44) plays from that
// world's bank as its sourced sounds do; nothing for the running world's own objects
void UsePcCopyBank(CSnd* psnd, const void* pcopy);
extern bool (*g_pfnPcSndBank)(const void* psrc, int sfxid, uint32_t* phbank, int* pisound, int* pkind);   // (PC addition, pcloader.h: non-null while a kept world loads: its music block's record)

void StartupSound();                                    // 0x1FCEA0
void StartupGameSound();
// The ambient sound objects (cid 0x2B; soundglue.cpp): activation (vt slot 23 0x12ED20 / slot 22 0x12EDE8)
struct LO;
void OnAmbientRegistered(LO* plo);
void OnAmbientUnregistered(LO* plo);
// a clone's records: the original's CloneLo (0x1A6730) copies the object, its records (+0xE8 / +0xF4) included
void CloneAmbientSoundRecords(const LO* source, LO* target);
void ResetAmbientSounds();  // StartupSound + the object model's hooks + the gait's footsteps (soundglue.cpp)
void ShutdownSound();
void ResetSoundMgr();                                   // 0x1FD700
void StopSoundsForMovie();                              // before a queued movie that leads to a world change
void LoadMusicFromBrx(CBinaryInputStream* pbis);        // 0x1FD0E8
void LoadSoundBank();                                   // 0x1FD5E8
void UpdateSoundMgr(float dt, float dtFrame);           // 0x1FD830 (game dt, CLOCK +0x40 frame step)
// (harness: the manager's clock runs by the frame step instead of the host's; set by --fixed-dt runs, which are not paced
// at real time, so the music's play time (the dances' beats) stays in step with the frames: 59.94 frames a second)
extern bool g_fSndFrameClock;

CSndPlain* PsndNewPlain(int sfxid);                     // 0x1FBB90
CSndVoice* PsndNewVoice(SNDSTREAM* pstream, uint32_t flags);  // 0x1FC138
// Starts the world's sound id sfxid (0x1FBB90 + volume / pan / source + Start); the caller may keep a CSndRef.
CSnd* PsndStartSound(int sfxid, float gVol = 1.0f, int pan = 0, void* psrc = nullptr,
                     const glm::vec3* ppos = nullptr);
CSndVoice* PsndPushMusic(int idMusic);                  // 0x1FE150
void PopMusic(int idMusic);                             // 0x1FE310
float TMusicTop();                                      // 0x1FE3F8
int IdMusicTop(int* pcStack = nullptr);                 // (native: the top music's id, -1 none; skipcut.h's check)
CSndVoice* PsndStartDialog(int idDialog, int fQuiet = 0);  // 0x1FE5F0
void StopDialog();                                      // 0x1FE738
// orig: 0x1A3078 / 0x1A30D0: the cut scenes' subtitle box shown (the subtitle reset first) / hidden (soundglue.cpp)
void ShowSubtitleBox();
void HideSubtitleBox();
bool FDialogPlaying(int idDialog);                      // 0x1FE848
float TDialog();                                        // 0x1FE888
void SetSoundMgrFlags(uint32_t mask, int fSet);         // 0x1FE5B8
void SetSoundPlaybackMode(int mode);                    // 0x1FE9C8
void SetSoundMasterVolume(float g);                     // 0x1FE9F8
void SetSoundChannelVolume(int ichan, int kind, float g);  // 0x1FEA60 (channel 0 ambience, 1 music, 2 dialog, 3 effects)
void ResetSoundVolumes();                               // 0x1FEDF0
// PC: the launcher's Music / Sound effects / Dialogue sliders (0..1, the shared settings; linear output factors on
// channel 1, channels 0 and 3, channel 2), on top of the game's own volumes (its options' kind 1 included)
void SetSoundUserVolumes(float gMusic, float gEffects, float gDialog);
void SendSoundChannelVolume(int ichan);                 // (part of 0x1FEA60: the product sent to 989snd)
void StepSoundMood();                                   // 0x1FEED0

// The dialog's subtitle state (0x4435F0: {the line's id (its text id), the speaker's colour, flags}): set when a
// line starts unless quiet (0x227728), reset (0x227788; SCPT 0x46). The HUD's subtitle box (a screen object's
// string kind 1 id 0x2EF, font_brx 0x215xxx) shows StrDialogSubtitle(): the text revealed with the line's time.
struct DIALOGSUB
{
    int id = -1;
    uint32_t rgba = 0x7F666666;  // the PS2's RGBA (0x80 = full)
    uint32_t grf = 0;
};
extern DIALOGSUB g_dialogsub;
uint32_t RgbaDialogSpeaker(int oidSpeaker);   // 0x2275B8
void SetDialogSubtitle(int id, int oidSpeaker);  // 0x227728
void ResetDialogSubtitle();                      // 0x227788
// 0x2277D0 + 0x227820: the subtitle's text now (the localized string of the line's id, cut in proportion to the
// line's time, then a blinking '_'; pieces counted as characters here); false when there is none.
bool FDialogSubtitle(std::string* pstr, uint32_t* prgba);
// 0x2277D0: the line's whole text (the reader then wraps it, 0x215F40); 0x227820: that wrapped text cut in proportion
// to the line's time, then the blinking '|'
bool FDialogSubtitleText(std::string* pstr);
std::string StrDialogSubtitleTyped(const std::string& strWrapped);

SNDSTREAM* PstreamFindDialog(int id);                   // 0x1FCFA0
SNDSTREAM* PstreamFindMusic(int id);                    // 0x1FCFF8
int IsoundFromSfxid(int sfxid);                         // 0x1FD028 (-1 none)
// The resident ambience's voice for a world target id (0x1FE990: its index in the block's target ids, -1 none).
// The world's ambience objects (0x12EC50) add their falloffs with it:
//   static_cast<CSndVoice*>(g_sound.sndResident.get())->AddVoiceFalloff(IvoiceFromTarget(id), palo, s0, s1, g)
int IvoiceFromTarget(int id);

// --list-sounds / --play-music / --play-sfx / --play-dialog
int ListSoundsCommand(const std::string& world, const std::string& dirMaps);
int PlayMusicCommand(const std::string& world, const std::string& dirMaps, int idMusic, double tSeconds, int istem);
int PlaySfxCommand(const std::string& world, const std::string& dirMaps, int sfxid, double tSeconds);
int PlayDialogCommand(const std::string& world, const std::string& dirMaps, int idDialog, double tSeconds);
