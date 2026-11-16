#pragma once
// Only the XAudio2 surface used by ProjectCane is adapted. Game sequencing,
// sample ownership, envelopes, routing, and reverb remain in sound.cpp.
#include <FAPO.h>
#include <FAudio.h>
#include <FAudioFX.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#ifdef _WIN32
// Windows builds that use FAudio (Sly2-Release, Sly3-Release): the basic Win32 types and macros come from the SDK.
#include <windows.h>
#else
using HRESULT = int32_t;
using UINT32 = uint32_t;
using BYTE = uint8_t;
using ULONG = uint32_t;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = static_cast<int32_t>(0x80004005u);
constexpr HRESULT E_POINTER = static_cast<int32_t>(0x80004003u);
#if defined(__APPLE__)
#include <mach-o/dyld.h>   // (its enum DYLD_BOOL already names FALSE = 0 / TRUE = 1)
#else
constexpr int TRUE = 1, FALSE = 0;
#endif
inline bool FAILED(HRESULT r) { return r < 0; }
inline bool SUCCEEDED(HRESULT r) { return r >= 0; }
#endif
#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#endif
using WAVEFORMATEX = FAudioWaveFormatEx;
using XAUDIO2_BUFFER = FAudioBuffer;
using XAUDIO2_VOICE_DETAILS = FAudioVoiceDetails;
using XAUDIO2_VOICE_STATE = FAudioVoiceState;
using XAUDIO2FX_REVERB_I3DL2_PARAMETERS = FAudioFXReverbI3DL2Parameters;
using XAUDIO2FX_REVERB_PARAMETERS = FAudioFXReverbParameters9;
constexpr auto WAVE_FORMAT_PCM = FAUDIO_FORMAT_PCM;
constexpr auto XAUDIO2_DEFAULT_PROCESSOR = FAUDIO_DEFAULT_PROCESSOR;
constexpr auto XAUDIO2_END_OF_STREAM = FAUDIO_END_OF_STREAM;
constexpr auto XAUDIO2_LOOP_INFINITE = FAUDIO_LOOP_INFINITE;
constexpr auto XAUDIO2_MIN_FREQ_RATIO = FAUDIO_MIN_FREQ_RATIO;
constexpr auto XAUDIO2_DEFAULT_FREQ_RATIO = FAUDIO_DEFAULT_FREQ_RATIO;
constexpr uint32_t XAUDIO2_MAX_AUDIO_CHANNELS = 64;
constexpr uint32_t XAUDIO2_MIN_SAMPLE_RATE = 1000;
constexpr uint32_t XAUDIO2_MAX_SAMPLE_RATE = 200000;
#define XAUDIO2FX_I3DL2_PRESET_DEFAULT FAUDIOFX_I3DL2_PRESET_DEFAULT
#define XAUDIO2FX_I3DL2_PRESET_SMALLROOM FAUDIOFX_I3DL2_PRESET_SMALLROOM
#define XAUDIO2FX_I3DL2_PRESET_MEDIUMROOM FAUDIOFX_I3DL2_PRESET_MEDIUMROOM
#define XAUDIO2FX_I3DL2_PRESET_LARGEROOM FAUDIOFX_I3DL2_PRESET_LARGEROOM
#define XAUDIO2FX_I3DL2_PRESET_SEWERPIPE FAUDIOFX_I3DL2_PRESET_SEWERPIPE
#define XAUDIO2FX_I3DL2_PRESET_ROOM FAUDIOFX_I3DL2_PRESET_ROOM
inline void ReverbConvertI3DL2ToNative(const FAudioFXReverbI3DL2Parameters* p,
                                       FAudioFXReverbParameters9* native, int32_t seven) {
    ReverbConvertI3DL2ToNative9(p, native, seven);
}
struct IUnknown {
    FAPO* effect = nullptr;
    void Release() {
        effect->Release(effect);
        delete this;
    }
};
inline HRESULT XAudio2CreateReverb(IUnknown** result) {
    auto wrapper = std::make_unique<IUnknown>();
    auto r = FAudioCreateReverb9(&wrapper->effect, 0);
    if (!r) *result = wrapper.release();
    return static_cast<HRESULT>(r);
}
// Callbacks are accepted for source compatibility with the Windows XAudio2
// path; FAudio voices here are created without them (Sly 1 polls instead).
struct IXAudio2EngineCallback {
    virtual ~IXAudio2EngineCallback() = default;
    virtual void STDMETHODCALLTYPE OnProcessingPassStart() = 0;
    virtual void STDMETHODCALLTYPE OnProcessingPassEnd() = 0;
    virtual void STDMETHODCALLTYPE OnCriticalError(HRESULT error) = 0;
};
struct IXAudio2VoiceCallback {
    virtual ~IXAudio2VoiceCallback() = default;
    virtual void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 bytesRequired) = 0;
    virtual void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() = 0;
    virtual void STDMETHODCALLTYPE OnStreamEnd() = 0;
    virtual void STDMETHODCALLTYPE OnBufferStart(void* bufferContext) = 0;
    virtual void STDMETHODCALLTYPE OnBufferEnd(void* bufferContext) = 0;
    virtual void STDMETHODCALLTYPE OnLoopEnd(void* bufferContext) = 0;
    virtual void STDMETHODCALLTYPE OnVoiceError(void* bufferContext, HRESULT error) = 0;
};
struct XAUDIO2_VOICE_SENDS;
struct CaneAudioVoice {
    FAudioVoice* handle = nullptr;
    void GetVoiceDetails(FAudioVoiceDetails* d) { FAudioVoice_GetVoiceDetails(handle, d); }
    // The game re-sends unchanged volumes and pan matrices for every voice at
    // 240 Hz. FAudio takes the voice's send lock (held by the mixer while it
    // renders that voice) for each call, so skip calls whose value is bitwise
    // identical to the last one this voice accepted.
    HRESULT SetVolume(float v) {
        if (hasAppliedVolume && std::memcmp(&v, &appliedVolume, sizeof(v)) == 0) return 0;
        const auto r = static_cast<HRESULT>(FAudioVoice_SetVolume(handle, v, 0));
        appliedVolume = v;
        hasAppliedVolume = SUCCEEDED(r);
        return r;
    }
    HRESULT SetOutputMatrix(CaneAudioVoice* destination, uint32_t source, uint32_t channels,
                            const float* matrix) {
        FAudioVoice* const target = destination ? destination->handle : nullptr;
        const std::size_t count = static_cast<std::size_t>(source) * channels;
        AppliedMatrix* slot = nullptr;
        if (target && count <= AppliedMatrix::kMaxValues) {
            for (AppliedMatrix& applied : appliedMatrices)
                if (applied.valid && applied.destination == target) slot = &applied;
            if (slot && slot->source == source && slot->channels == channels &&
                std::memcmp(slot->values.data(), matrix, count * sizeof(float)) == 0)
                return 0;
            if (!slot)
                for (AppliedMatrix& applied : appliedMatrices)
                    if (!applied.valid) {
                        slot = &applied;
                        break;
                    }
        }
        const auto r = static_cast<HRESULT>(
            FAudioVoice_SetOutputMatrix(handle, target, source, channels, matrix, 0));
        if (slot) {
            slot->valid = SUCCEEDED(r);
            slot->destination = target;
            slot->source = source;
            slot->channels = channels;
            std::memcpy(slot->values.data(), matrix, count * sizeof(float));
        }
        return r;
    }
    void GetVolume(float* volume) { FAudioVoice_GetVolume(handle, volume); }
    HRESULT SetOutputVoices(const XAUDIO2_VOICE_SENDS* sends);
    HRESULT SetEffectParameters(uint32_t index, const void* data, uint32_t size) {
        return static_cast<HRESULT>(FAudioVoice_SetEffectParameters(handle, index, data, size, 0));
    }
    HRESULT EnableEffect(uint32_t index) {
        return static_cast<HRESULT>(FAudioVoice_EnableEffect(handle, index, 0));
    }
    HRESULT DisableEffect(uint32_t index) {
        return static_cast<HRESULT>(FAudioVoice_DisableEffect(handle, index, 0));
    }
    void DestroyVoice() {
        FAudioVoice_DestroyVoice(handle);
        delete this;
    }
    virtual ~CaneAudioVoice() = default;

private:
    struct AppliedMatrix {
        static constexpr std::size_t kMaxValues = 8;
        FAudioVoice* destination = nullptr;
        uint32_t source = 0;
        uint32_t channels = 0;
        std::array<float, kMaxValues> values{};
        bool valid = false;
    };
    float appliedVolume = 0.0f;
    bool hasAppliedVolume = false;
    std::array<AppliedMatrix, 2> appliedMatrices{};
};
struct IXAudio2MasteringVoice : CaneAudioVoice {};
struct IXAudio2SubmixVoice : CaneAudioVoice {};
struct IXAudio2SourceVoice : CaneAudioVoice {
    HRESULT Start() { return static_cast<HRESULT>(FAudioSourceVoice_Start(handle, 0, 0)); }
    HRESULT Stop() { return static_cast<HRESULT>(FAudioSourceVoice_Stop(handle, 0, 0)); }
    HRESULT FlushSourceBuffers() {
        return static_cast<HRESULT>(FAudioSourceVoice_FlushSourceBuffers(handle));
    }
    HRESULT SubmitSourceBuffer(const FAudioBuffer* buffer) {
        return static_cast<HRESULT>(FAudioSourceVoice_SubmitSourceBuffer(handle, buffer, nullptr));
    }
    HRESULT SetFrequencyRatio(float ratio) {
        if (hasAppliedRatio && std::memcmp(&ratio, &appliedRatio, sizeof(ratio)) == 0) return 0;
        const auto r = static_cast<HRESULT>(FAudioSourceVoice_SetFrequencyRatio(handle, ratio, 0));
        appliedRatio = ratio;
        hasAppliedRatio = SUCCEEDED(r);
        return r;
    }
    void GetState(FAudioVoiceState* state) { FAudioSourceVoice_GetState(handle, state, 0); }

private:
    float appliedRatio = 0.0f;
    bool hasAppliedRatio = false;
};
struct XAUDIO2_SEND_DESCRIPTOR {
    uint32_t Flags = 0;
    CaneAudioVoice* pOutputVoice = nullptr;
};
struct XAUDIO2_VOICE_SENDS {
    uint32_t SendCount = 0;
    XAUDIO2_SEND_DESCRIPTOR* pSends = nullptr;
};
struct XAUDIO2_EFFECT_DESCRIPTOR {
    IUnknown* pEffect = nullptr;
    int32_t InitialState = 0;
    uint32_t OutputChannels = 0;
};
struct XAUDIO2_EFFECT_CHAIN {
    uint32_t EffectCount = 0;
    XAUDIO2_EFFECT_DESCRIPTOR* pEffectDescriptors = nullptr;
};
using IXAudio2Voice = CaneAudioVoice;
inline std::vector<FAudioSendDescriptor> CaneAudioSends(const XAUDIO2_VOICE_SENDS* sends) {
    std::vector<FAudioSendDescriptor> result;
    if (sends)
        for (uint32_t i = 0; i < sends->SendCount; ++i)
            result.push_back({sends->pSends[i].Flags, sends->pSends[i].pOutputVoice->handle});
    return result;
}
inline HRESULT CaneAudioVoice::SetOutputVoices(const XAUDIO2_VOICE_SENDS* sends) {
    auto descriptors = CaneAudioSends(sends);
    FAudioVoiceSends list{static_cast<uint32_t>(descriptors.size()), descriptors.data()};
    // A new routing invalidates the cached per-destination matrices.
    for (AppliedMatrix& applied : appliedMatrices) applied.valid = false;
    return static_cast<HRESULT>(FAudioVoice_SetOutputVoices(handle, sends ? &list : nullptr));
}
struct IXAudio2 {
    FAudio* handle = nullptr;
    HRESULT RegisterForCallbacks(IXAudio2EngineCallback*) { return S_OK; }
    void UnregisterForCallbacks(IXAudio2EngineCallback*) {}
    // Defaults keep the original fixed 2 ch / 48 kHz mix (Sly 2 relies on it);
    // Sly 1 passes its own format.
    // (deviceIndex: FAudio's, 0 the default device, else SDL's output device index + 1; Sly2-Release audio.cpp)
    HRESULT CreateMasteringVoice(IXAudio2MasteringVoice** result, uint32_t channels = 2,
                                 uint32_t sampleRate = 48000, uint32_t deviceIndex = 0) {
        auto voice = std::make_unique<IXAudio2MasteringVoice>();
        auto r = FAudio_CreateMasteringVoice(handle, &voice->handle, channels, sampleRate, 0, deviceIndex, nullptr);
        if (!r) *result = voice.release();
        return static_cast<HRESULT>(r);
    }
    HRESULT CreateSubmixVoice(IXAudio2SubmixVoice** result, uint32_t channels, uint32_t rate,
                              uint32_t flags, uint32_t stage, const XAUDIO2_VOICE_SENDS* sends,
                              const XAUDIO2_EFFECT_CHAIN* chain) {
        auto sendDescriptors = CaneAudioSends(sends);
        FAudioVoiceSends sendList{static_cast<uint32_t>(sendDescriptors.size()), sendDescriptors.data()};
        std::vector<FAudioEffectDescriptor> descriptors;
        if (chain)
            for (uint32_t i = 0; i < chain->EffectCount; ++i) {
                auto& e = chain->pEffectDescriptors[i];
                descriptors.push_back({e.pEffect->effect, e.InitialState, e.OutputChannels});
            }
        FAudioEffectChain effects{static_cast<uint32_t>(descriptors.size()), descriptors.data()};
        auto voice = std::make_unique<IXAudio2SubmixVoice>();
        auto r = FAudio_CreateSubmixVoice(handle, &voice->handle, channels, rate, flags, stage,
                                          sends ? &sendList : nullptr, chain ? &effects : nullptr);
        if (!r) *result = voice.release();
        return static_cast<HRESULT>(r);
    }
    HRESULT CreateSourceVoice(IXAudio2SourceVoice** result, const WAVEFORMATEX* format,
                              uint32_t flags = 0, float maximum = 2.0f, IXAudio2VoiceCallback* = nullptr,
                              const XAUDIO2_VOICE_SENDS* sends = nullptr, void* = nullptr) {
        auto descriptors = CaneAudioSends(sends);
        FAudioVoiceSends sendList{static_cast<uint32_t>(descriptors.size()), descriptors.data()};
        auto voice = std::make_unique<IXAudio2SourceVoice>();
        auto r = FAudio_CreateSourceVoice(handle, &voice->handle, format, flags, maximum, nullptr,
                                          sends ? &sendList : nullptr, nullptr);
        if (!r) *result = voice.release();
        return static_cast<HRESULT>(r);
    }
    void Release() {
        FAudio_Release(handle);
        delete this;
    }
};
inline HRESULT XAudio2Create(IXAudio2** result, uint32_t flags, uint32_t processor) {
    auto engine = std::make_unique<IXAudio2>();
    auto r = FAudioCreate(&engine->handle, flags, processor);
    if (!r) *result = engine.release();
    return static_cast<HRESULT>(r);
}
