#pragma once
// XAudio2 on the Switch: the subset Sly1-Release (sound.cpp) and Sly2-Release (audio.cpp, snd989.cpp) use, mixed by
// platform/switch/audio_switch.cpp into the console's audio output (audout, 48 kHz stereo). No FAudio, no SDL.
//
// The model is ProjectCane's Switch mixer: source voices (8/16/24/32-bit PCM or 32-bit float, mono or stereo, any rate,
// queued buffers with XAudio2's play and loop regions, the frequency ratio), submix voices (stereo, processing stages,
// the I3DL2 reverb as a feedback delay network), up to four sends per voice with an output matrix each, the mastering
// voice's volume, and the voice and engine callbacks. The output matrices follow XAudio2's layout:
// matrix[SourceChannels * destination + source].
//
// Thread safety: every call takes the engine's lock, which the mixer holds for a whole pass, so DestroyVoice returns
// once the mixer no longer reads the voice's buffers. The lock is recursive: the callbacks run on the mixer thread and
// may call back in (SubmitSourceBuffer from OnBufferEnd), as XAudio2 allows. A voice's levels (volume, send matrices,
// frequency ratio) have their own lock, which the mixer takes only to copy them, so the games' per-frame level changes
// never wait for a pass.
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

using BYTE = uint8_t;
using UINT32 = uint32_t;
using UINT64 = uint64_t;
using ULONG = uint32_t;
using HRESULT = int32_t;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = static_cast<int32_t>(0x80004005u);
constexpr HRESULT E_POINTER = static_cast<int32_t>(0x80004003u);
constexpr HRESULT E_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int TRUE = 1, FALSE = 0;
inline bool FAILED(HRESULT r) { return r < 0; }
inline bool SUCCEEDED(HRESULT r) { return r >= 0; }
#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#endif

constexpr uint16_t WAVE_FORMAT_PCM = 1;
constexpr uint16_t WAVE_FORMAT_IEEE_FLOAT = 3;
constexpr uint16_t WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
constexpr UINT32 XAUDIO2_DEFAULT_PROCESSOR = 0;
constexpr UINT32 XAUDIO2_DEFAULT_CHANNELS = 0;
constexpr UINT32 XAUDIO2_DEFAULT_SAMPLERATE = 0;
constexpr UINT32 XAUDIO2_COMMIT_NOW = 0;
constexpr UINT32 XAUDIO2_END_OF_STREAM = 0x40;
constexpr UINT32 XAUDIO2_LOOP_INFINITE = 255;
constexpr UINT32 XAUDIO2_VOICE_NOSAMPLESPLAYED = 0x0100;
constexpr UINT32 XAUDIO2_MAX_AUDIO_CHANNELS = 64;
constexpr UINT32 XAUDIO2_MIN_SAMPLE_RATE = 1000;
constexpr UINT32 XAUDIO2_MAX_SAMPLE_RATE = 200000;
constexpr float XAUDIO2_DEFAULT_FREQ_RATIO = 2.0f;
constexpr float XAUDIO2_MIN_FREQ_RATIO = 1.0f / 1024.0f;
constexpr float XAUDIO2_MAX_FREQ_RATIO = 1024.0f;

struct IXAudio2EngineCallback
{
    virtual ~IXAudio2EngineCallback() = default;
    virtual void OnProcessingPassStart() = 0;
    virtual void OnProcessingPassEnd() = 0;
    virtual void OnCriticalError(HRESULT error) = 0;
};
struct IXAudio2VoiceCallback
{
    virtual ~IXAudio2VoiceCallback() = default;
    virtual void OnVoiceProcessingPassStart(UINT32 bytesRequired) = 0;
    virtual void OnVoiceProcessingPassEnd() = 0;
    virtual void OnStreamEnd() = 0;
    virtual void OnBufferStart(void* context) = 0;
    virtual void OnBufferEnd(void* context) = 0;
    virtual void OnLoopEnd(void* context) = 0;
    virtual void OnVoiceError(void* context, HRESULT error) = 0;
};

struct WAVEFORMATEX
{
    uint16_t wFormatTag = 0, nChannels = 0;
    uint32_t nSamplesPerSec = 0, nAvgBytesPerSec = 0;
    uint16_t nBlockAlign = 0, wBitsPerSample = 0, cbSize = 0;
};
struct XAUDIO2_BUFFER
{
    UINT32 Flags = 0, AudioBytes = 0;
    const BYTE* pAudioData = nullptr;
    UINT32 PlayBegin = 0, PlayLength = 0, LoopBegin = 0, LoopLength = 0, LoopCount = 0;
    void* pContext = nullptr;
};
struct XAUDIO2_VOICE_STATE
{
    void* pCurrentBufferContext = nullptr;
    UINT32 BuffersQueued = 0;
    UINT64 SamplesPlayed = 0;
};
struct XAUDIO2_VOICE_DETAILS { UINT32 CreationFlags = 0, ActiveFlags = 0, InputChannels = 0, InputSampleRate = 0; };

// (XAudio2CreateReverb's object: the submix voice that carries it runs the reverb)
struct IUnknown
{
    virtual ~IUnknown() = default;
    void Release() { delete this; }
};
struct XAUDIO2_EFFECT_DESCRIPTOR { IUnknown* pEffect = nullptr; int InitialState = 0; UINT32 OutputChannels = 0; };
struct XAUDIO2_EFFECT_CHAIN { UINT32 EffectCount = 0; XAUDIO2_EFFECT_DESCRIPTOR* pEffectDescriptors = nullptr; };
struct XAUDIO2FX_REVERB_I3DL2_PARAMETERS { float WetDryMix = 100.0f, DecayTime = 1.0f, DecayHFRatio = 0.5f; };
struct XAUDIO2FX_REVERB_PARAMETERS { float DecayTime = 1.0f, DecayHFRatio = 0.5f; };
inline constexpr XAUDIO2FX_REVERB_I3DL2_PARAMETERS XAUDIO2FX_I3DL2_PRESET_DEFAULT{100.0f, 1.49f, 0.83f};
inline constexpr XAUDIO2FX_REVERB_I3DL2_PARAMETERS XAUDIO2FX_I3DL2_PRESET_ROOM{100.0f, 0.4f, 0.83f};
inline constexpr XAUDIO2FX_REVERB_I3DL2_PARAMETERS XAUDIO2FX_I3DL2_PRESET_SMALLROOM{100.0f, 0.7f, 0.7f};
inline constexpr XAUDIO2FX_REVERB_I3DL2_PARAMETERS XAUDIO2FX_I3DL2_PRESET_MEDIUMROOM{100.0f, 1.3f, 0.7f};
inline constexpr XAUDIO2FX_REVERB_I3DL2_PARAMETERS XAUDIO2FX_I3DL2_PRESET_LARGEROOM{100.0f, 2.3f, 0.7f};
inline constexpr XAUDIO2FX_REVERB_I3DL2_PARAMETERS XAUDIO2FX_I3DL2_PRESET_SEWERPIPE{100.0f, 2.81f, 0.14f};
inline void ReverbConvertI3DL2ToNative(const XAUDIO2FX_REVERB_I3DL2_PARAMETERS* in, XAUDIO2FX_REVERB_PARAMETERS* out, int = 0)
{
    out->DecayTime = in->DecayTime;
    out->DecayHFRatio = in->DecayHFRatio;
}
inline HRESULT XAudio2CreateReverb(IUnknown** out, UINT32 = 0)
{
    if (!out) return E_POINTER;
    *out = new IUnknown;
    return S_OK;
}

struct IXAudio2;
struct IXAudio2Voice;
struct XAUDIO2_SEND_DESCRIPTOR { UINT32 Flags = 0; IXAudio2Voice* pOutputVoice = nullptr; };
struct XAUDIO2_VOICE_SENDS { UINT32 SendCount = 0; XAUDIO2_SEND_DESCRIPTOR* pSends = nullptr; };

struct IXAudio2Voice
{
    // One output of the voice: the destination's stereo input and the levels into it (matrix[source + channels * d]).
    struct Send
    {
        IXAudio2Voice* destination = nullptr;
        std::array<float, 4> matrix{};
    };
    // (the mixer) the levels a pass mixes the voice with, copied as it comes to the voice
    struct Levels
    {
        float volume = 1.0f;
        std::array<Send, 4> sends{};
        unsigned sendCount = 0;
    };
    IXAudio2* engine;
    UINT32 channels, sampleRate;
    mutable std::mutex levelsLock;      // volume, sends, sendCount (and a source voice's ratio)
    float volume = 1.0f;
    std::array<Send, 4> sends{};
    unsigned sendCount = 0;
    bool dead = false;                  // destroyed during a mix pass (freed at its end)

    IXAudio2Voice(IXAudio2* owner, UINT32 count, UINT32 rate) : engine(owner), channels(count), sampleRate(rate) {}
    virtual ~IXAudio2Voice() = default;
    HRESULT SetVolume(float value, UINT32 = XAUDIO2_COMMIT_NOW);
    void GetVolume(float* value);
    HRESULT SetOutputMatrix(IXAudio2Voice* destination, UINT32 sourceChannels, UINT32 destinationChannels, const float* levels,
                            UINT32 = XAUDIO2_COMMIT_NOW);
    HRESULT SetOutputVoices(const XAUDIO2_VOICE_SENDS* outputs);
    void GetVoiceDetails(XAUDIO2_VOICE_DETAILS* details);
    virtual void DestroyVoice() = 0;
    // (the mixer) the levels now
    Levels CurrentLevels() const;
    // (the mixer) one stereo frame into the sends' destinations; `master` is the output buffer
    void Route(const Levels& levels, float left, float right, unsigned frame, float* master) const;
};

struct IXAudio2MasteringVoice : IXAudio2Voice
{
    using IXAudio2Voice::IXAudio2Voice;
    void DestroyVoice() override;
};

struct IXAudio2SubmixVoice : IXAudio2Voice
{
    struct Delay
    {
        std::vector<float> data;
        size_t position = 0;
        float filtered = 0.0f, feedback = 0.7f;
    };
    std::array<Delay, 8> delays;
    std::vector<float> input;           // this pass's stereo input (frames x 2)
    float damping = 0.4f;
    UINT32 stage = 0;
    bool effect = false;                // the reverb runs (the voice was created with an enabled effect)

    IXAudio2SubmixVoice(IXAudio2* owner, UINT32 count, UINT32 rate);
    HRESULT SetEffectParameters(UINT32 effect, const void* parameters, UINT32 size, UINT32 = XAUDIO2_COMMIT_NOW);
    HRESULT EnableEffect(UINT32, UINT32 = XAUDIO2_COMMIT_NOW);
    HRESULT DisableEffect(UINT32, UINT32 = XAUDIO2_COMMIT_NOW);
    void DestroyVoice() override;
    // (the mixer) the reverb's wet output of one input frame
    void Reverb(float left, float right, float& outLeft, float& outRight);
};

struct IXAudio2SourceVoice : IXAudio2Voice
{
    WAVEFORMATEX format;
    IXAudio2VoiceCallback* callback = nullptr;
    std::deque<XAUDIO2_BUFFER> queue;
    uint64_t position = 0;              // 32.32 fixed point: frames into the buffer at the queue's front
    uint64_t played = 0;                // 32.32 fixed point: source frames played since the last flush
    UINT32 loopsLeft = 0;
    float ratio = 1.0f, maxRatio = 2.0f;   // (ratio: under levelsLock)
    bool playing = false;
    bool bufferStarted = false;         // (OnBufferStart sent for the buffer at the front)

    IXAudio2SourceVoice(IXAudio2* owner, const WAVEFORMATEX& wave, float maxFrequencyRatio);
    HRESULT Start(UINT32 = 0, UINT32 = XAUDIO2_COMMIT_NOW);
    HRESULT Stop(UINT32 = 0, UINT32 = XAUDIO2_COMMIT_NOW);
    HRESULT FlushSourceBuffers();
    HRESULT SubmitSourceBuffer(const XAUDIO2_BUFFER* buffer, const void* = nullptr);
    HRESULT Discontinuity() { return S_OK; }
    HRESULT SetFrequencyRatio(float value, UINT32 = XAUDIO2_COMMIT_NOW);
    void GetState(XAUDIO2_VOICE_STATE* state, UINT32 = 0);
    void DestroyVoice() override;
    // (the mixer) up to `frames` output frames into the sends
    void Mix(float* master, unsigned frames);
};

struct IXAudio2
{
    void* native = nullptr;             // the output device (audio_switch.cpp)
    std::vector<IXAudio2SourceVoice*> voices;
    std::vector<IXAudio2SubmixVoice*> submixes;   // in processing-stage order
    std::vector<IXAudio2EngineCallback*> callbacks;
    std::vector<IXAudio2Voice*> graveyard;        // voices destroyed during the pass running now
    IXAudio2MasteringVoice* master = nullptr;
    bool mixing = false;

    HRESULT CreateMasteringVoice(IXAudio2MasteringVoice** out, UINT32 channels = XAUDIO2_DEFAULT_CHANNELS,
                                 UINT32 rate = XAUDIO2_DEFAULT_SAMPLERATE, UINT32 = 0, UINT32 = 0, const void* = nullptr, int = 0);
    HRESULT CreateSubmixVoice(IXAudio2SubmixVoice** out, UINT32 channels, UINT32 rate, UINT32 flags = 0, UINT32 stage = 0,
                              const XAUDIO2_VOICE_SENDS* sends = nullptr, const XAUDIO2_EFFECT_CHAIN* effects = nullptr);
    HRESULT CreateSourceVoice(IXAudio2SourceVoice** out, const WAVEFORMATEX* format, UINT32 flags = 0,
                              float maxRatio = XAUDIO2_DEFAULT_FREQ_RATIO, IXAudio2VoiceCallback* callback = nullptr,
                              const XAUDIO2_VOICE_SENDS* sends = nullptr, const XAUDIO2_EFFECT_CHAIN* effects = nullptr);
    HRESULT RegisterForCallbacks(IXAudio2EngineCallback* callback);
    void UnregisterForCallbacks(IXAudio2EngineCallback* callback);
    void Release();
    void Lock();
    void Unlock();
    // (the output thread) one pass: `frames` stereo float frames into `output`
    void Mix(float* output, unsigned frames);
    void NotifyCriticalError(HRESULT error);
};
HRESULT XAudio2Create(IXAudio2** out, UINT32 flags = 0, UINT32 processor = XAUDIO2_DEFAULT_PROCESSOR);
