#pragma once

// The host audio device: XAudio2 (Windows) or FAudio on SDL2 (platform/audio_linux.h), as Sly1-Release's
// sound.cpp uses it. Everything the PS2's SPU2 mixed (989snd's voices, spiop's streams, the movie sound) is a
// source voice here, all feeding one 48 kHz stereo mastering voice.
//
// SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=out.raw (SDL2) records the mix (f32 stereo 48 kHz) for checks.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

// The engine and its mastering voice, created on first use; false when there is no audio device.
bool FEnsureAudio();
// The level of the whole mix: the SPU2's master volume (989snd group 16, 0..1) times kOutputGain.
void SetOutputVolume(float g);
// (PC: the cutscene skip's fast-forward) the whole mix silenced, or back at its volume
void SetOutputMute(bool fMute);
// (PC: the original's Speakers: Mono, Settings.ini) every voice's left and right mixed together, from the next level
// change of each voice on (SetVoiceLevels); off (Surround / Stereo, the default): the voices' own stereo
void SetMonoOutput(bool fMono);
// The output level of the SPU2 mix relative to 989snd's register volumes (0x3FFF = 1.0): the banked effects
// match the reference port's 989snd at 1.0 (a front-end menu sound in its SLY2_AUDIO_WAV: gain 0.96..1.0 against
// the same sound rendered alone by its 989snd).
constexpr float kOutputGain = 1.0f;
// The streams (spiop's music / ambience / dialog, the movie sound) sound at half the level 989snd's volumes give,
// in the reference port's output (the boot movie 2056: 0.248 of the decoded ADPCM where the movie voices'
// volumes give 0.5; world 5's music stem: 0.46 where they give 1.0). Sly1-Release's VAG streams use the same
// 0x3FFC / 0x7FFF. (Where the reference's halving comes from is not found yet.)
constexpr float kStreamGain = 0.5f;
void ShutdownAudio();
extern IXAudio2* g_pxaudio2;
extern IXAudio2MasteringVoice* g_pmastervoice;

// One lock for all sound state (989snd, spiop, the game's sound objects): the 240 Hz sound thread and the game
// thread both take it.
extern std::recursive_mutex g_mtxSound;

// (PC) a game voice (989snd's, a stream's; 16-bit PCM): it feeds the output bus, which feeds the mastering voice (the
// device can then change while it plays)
bool FCreateGameSourceVoice(IXAudio2SourceVoice** ppvoice, int cch, int rate, float frqMax);

// ---- (PC) the audio devices by name (notes/AUDIO_2026-11-10.md; platform/audio_devices.h): "" the system default.
// A chosen device that is missing plays on (records from) the default, and is taken again when it is back.
namespace cane::audio { struct DeviceList; }
void SetAudioDevices(const std::string& output, const std::string& input, const std::string& headsetOutput);
// From the game loop (every 2 s; fNow: at once): follows unplugged / returning devices; no work while every device is
// the default and the USB headset is off
void PollAudioDevices(bool fNow = false);
// The devices to choose from (desktop builds; ListAudioDevices().available false elsewhere) and whether the choice
// is shown at all (not on the Switch or Android)
cane::audio::DeviceList ListAudioDevices();
bool FAudioDeviceChoiceShown();
std::string SzAudioOutputOpen();   // the output device open ("" the default)
// The USB headset (the original's option; sound.cpp): its microphone recorded while on, and its level as spiop.irx
// computes it (the RMS of the last 1024 samples, 0..1); its speaker (the Headset Output Device) open
void SetMicCapture(bool fOn);
float GMicLevel();
bool FHeadsetOutputReady();

// A mono voice's (or a stereo voice's two channels') levels into the stereo mix; 1.0 is the SPU2's 0x3FFF.
void SetVoiceLevels(IXAudio2SourceVoice* pvoice, int cchSource, float gLeft, float gRight);

// A streamed 16-bit PCM voice (mono or interleaved stereo) fed by the caller: spiop's tracks and the movie
// sound. Push copies the samples into a ring of buffers the device reads in place.
class CPcmStream
{
public:
    CPcmStream() = default;
    CPcmStream(const CPcmStream&) = delete;
    CPcmStream& operator=(const CPcmStream&) = delete;
    ~CPcmStream() { Close(); }

    // (fHeadset: on the USB headset's speaker, the Headset Output Device (FHeadsetOutputReady); false when it is not open)
    bool FOpen(int cch, int rate, float frqMax = 4.0f, bool fHeadset = false);
    void Close();
    [[nodiscard]] bool FIsOpen() const { return m_pvoice != nullptr; }

    // Queues csample frames (cch samples each); false when every buffer is still in use.
    bool FPush(const int16_t* asample, std::size_t cframe, bool fEnd = false);
    // Buffers queued in the device (not played yet).
    [[nodiscard]] int CBufferQueued() const;
    // Frames the device has consumed since FOpen.
    [[nodiscard]] uint64_t CFramePlayed() const;
    [[nodiscard]] bool FCanPush() const { return CBufferQueued() < kcBuffer; }

    void Start();
    void Stop();
    void SetLevels(float gLeft, float gRight);
    // Stereo: {left out from left, left out from right, right out from left, right out from right}.
    void SetMatrix(const float matrix[4]);
    void SetFrequencyRatio(float r);

    int m_cch = 0;
    int m_rate = 0;
    static constexpr int kcBuffer = 4;

private:
    IXAudio2SourceVoice* m_pvoice = nullptr;
    bool m_fHeadset = false;
    std::FILE* m_pfTap = nullptr;  // (test: SLY2N_PCMTAP=dir: every frame pushed, one raw file per stream)
    std::vector<int16_t> m_aab[kcBuffer];
    int m_iab = 0;
};

// SPU2 ADPCM: one 16-byte block (28 samples) with the predictor history in *ph1 / *ph2 (kept across blocks,
// as the SPU2 does, also over a loop jump).
void DecodeAdpcmBlock(const uint8_t* pb, int16_t* asample, int* ph1, int* ph2);
