// See audio_switch.h.
#include "audio_switch.h"
#include "runtime.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
constexpr unsigned kFrames = 1024;       // per output buffer (21.3 ms at 48 kHz)
constexpr unsigned kBuffers = 3;
constexpr unsigned kOutputRate = 48000;
constexpr double kFixed = 4294967296.0;  // 32.32 fixed point

// The output device behind an engine.
struct Device
{
    IXAudio2* engine = nullptr;
    RMutex lock{};
    Thread thread{};
    bool threadCreated = false, threadStarted = false, service = false;
    std::atomic<bool> stop{false};
    std::atomic<Result> error{0};
    std::array<AudioOutBuffer, kBuffers> buffers{};
    std::array<float, kFrames * 2> mix{};
};

struct EngineLock
{
    IXAudio2* engine;
    explicit EngineLock(IXAudio2* e) : engine(e) { engine->Lock(); }
    ~EngineLock() { engine->Unlock(); }
    EngineLock(const EngineLock&) = delete;
    EngineLock& operator=(const EngineLock&) = delete;
};

int16_t ToPcm16(float sample)
{
    if (!(sample > -1.0f)) return sample <= -1.0f ? INT16_MIN : 0;   // (NaN: silence)
    if (sample >= 1.0f) return INT16_MAX;
    return int16_t(std::lrint(sample * 32767.0f));
}

// A buffer's play region and loop region, in frames.
struct Region
{
    UINT32 end, loopBegin, loopEnd;
};
Region RegionOf(const XAUDIO2_BUFFER& b, UINT32 blockAlign)
{
    const UINT32 total = b.AudioBytes / blockAlign;
    const UINT32 end = b.PlayLength ? b.PlayBegin + b.PlayLength : total;
    return {end, b.LoopBegin, b.LoopLength ? b.LoopBegin + b.LoopLength : end};
}

// Sample readers: 16-bit PCM inline (989snd's voices, the dialog, the music), the other formats by a switch.
struct Pcm16
{
    static float Read(const WAVEFORMATEX& f, const BYTE* data, UINT32 frame, UINT32 channel)
    {
        int16_t v;
        std::memcpy(&v, data + size_t(frame) * f.nBlockAlign + channel * 2u, 2);
        return float(v) * (1.0f / 32768.0f);
    }
};
struct AnyPcm
{
    static float Read(const WAVEFORMATEX& f, const BYTE* data, UINT32 frame, UINT32 channel)
    {
        const BYTE* p = data + size_t(frame) * f.nBlockAlign + channel * (f.wBitsPerSample / 8u);
        if (f.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        {
            float v;
            std::memcpy(&v, p, 4);
            return std::isfinite(v) ? v : 0.0f;
        }
        switch (f.wBitsPerSample)
        {
        case 8: return (int(*p) - 128) * (1.0f / 128.0f);
        case 16: { int16_t v; std::memcpy(&v, p, 2); return float(v) * (1.0f / 32768.0f); }
        case 24:
        {
            int32_t v = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);
            if (v & 0x800000) v |= int32_t(0xFF000000u);
            return float(v) * (1.0f / 8388608.0f);
        }
        case 32: { int32_t v; std::memcpy(&v, p, 4); return float(double(v) * (1.0 / 2147483648.0)); }
        default: return 0.0f;
        }
    }
};

std::vector<IXAudio2SourceVoice*> g_passVoices;   // (the mixer thread's snapshot of the voices)
std::vector<IXAudio2SubmixVoice*> g_passSubmixes;

bool KnownDestination(IXAudio2* engine, IXAudio2Voice* v)
{
    return v && !v->dead && (v == engine->master ||
                             std::find(engine->submixes.begin(), engine->submixes.end(), v) != engine->submixes.end());
}
std::array<float, 4> DefaultMatrix(UINT32 channels)
{
    // mono: both sides at full level; stereo: left to left, right to right
    return channels == 1 ? std::array<float, 4>{1.0f, 1.0f, 0.0f, 0.0f} : std::array<float, 4>{1.0f, 0.0f, 0.0f, 1.0f};
}
}  // namespace

// ------------------------------------------------------------------------------------------------ voices
HRESULT IXAudio2Voice::SetVolume(float value, UINT32)
{
    if (!std::isfinite(value) || value < 0.0f) return E_INVALIDARG;
    std::lock_guard<std::mutex> levels(levelsLock);
    volume = value;
    return S_OK;
}
void IXAudio2Voice::GetVolume(float* value)
{
    if (!value) return;
    std::lock_guard<std::mutex> levels(levelsLock);
    *value = volume;
}
HRESULT IXAudio2Voice::SetOutputVoices(const XAUDIO2_VOICE_SENDS* outputs)
{
    // (no sends given: the master)
    const unsigned count = outputs ? outputs->SendCount : 1;
    if (count > sends.size() || (outputs && count && !outputs->pSends)) return E_INVALIDARG;
    EngineLock lock(engine);   // (the destinations are the engine's voices)
    std::lock_guard<std::mutex> levels(levelsLock);
    std::array<Send, 4> replacement{};
    for (unsigned i = 0; i < count; ++i)
    {
        IXAudio2Voice* destination = outputs ? outputs->pSends[i].pOutputVoice : engine->master;
        if (destination == this || !KnownDestination(engine, destination)) return E_INVALIDARG;
        replacement[i].destination = destination;
        replacement[i].matrix = DefaultMatrix(channels);
        for (unsigned j = 0; j < sendCount; ++j)   // (a destination kept keeps its levels)
            if (sends[j].destination == destination) replacement[i].matrix = sends[j].matrix;
    }
    sends = replacement;
    sendCount = count;
    return S_OK;
}
HRESULT IXAudio2Voice::SetOutputMatrix(IXAudio2Voice* destination, UINT32 sourceChannels, UINT32 destinationChannels,
                                       const float* levels, UINT32)
{
    if (!levels || sourceChannels != channels || sourceChannels > 2 || destinationChannels != 2) return E_INVALIDARG;
    for (UINT32 i = 0; i < sourceChannels * destinationChannels; ++i)
        if (!std::isfinite(levels[i])) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(levelsLock);
    if (!destination) destination = sendCount == 1 ? sends[0].destination : engine->master;
    for (unsigned i = 0; i < sendCount; ++i)
        if (sends[i].destination == destination)
        {
            std::copy_n(levels, sourceChannels * destinationChannels, sends[i].matrix.begin());
            return S_OK;
        }
    return E_INVALIDARG;
}
void IXAudio2Voice::GetVoiceDetails(XAUDIO2_VOICE_DETAILS* details)
{
    if (!details) return;
    *details = {};
    details->InputChannels = channels;
    details->InputSampleRate = sampleRate;
}
IXAudio2Voice::Levels IXAudio2Voice::CurrentLevels() const
{
    std::lock_guard<std::mutex> lock(levelsLock);
    return {volume, sends, sendCount};
}
void IXAudio2Voice::Route(const Levels& levels, float left, float right, unsigned frame, float* master) const
{
    for (unsigned i = 0; i < levels.sendCount; ++i)
    {
        const Send& send = levels.sends[i];
        float* out = send.destination == engine->master ? master
                   : static_cast<IXAudio2SubmixVoice*>(send.destination)->input.data();
        float* o = out + size_t(frame) * 2;
        const std::array<float, 4>& m = send.matrix;
        if (channels == 1)
        {
            o[0] += left * m[0];
            o[1] += left * m[1];
        }
        else
        {
            o[0] += left * m[0] + right * m[1];
            o[1] += left * m[2] + right * m[3];
        }
    }
}

void IXAudio2MasteringVoice::DestroyVoice()
{
    EngineLock lock(engine);
    engine->master = nullptr;
    dead = true;
    if (engine->mixing) engine->graveyard.push_back(this);
    else delete this;
}

IXAudio2SubmixVoice::IXAudio2SubmixVoice(IXAudio2* owner, UINT32 count, UINT32 rate) : IXAudio2Voice(owner, count, rate)
{
    static constexpr size_t kLengths[] = {1557, 1617, 1491, 1422, 1580, 1640, 1514, 1445};
    for (size_t i = 0; i < delays.size(); ++i) delays[i].data.assign(kLengths[i], 0.0f);
    input.assign(size_t(kFrames) * 2, 0.0f);
}
HRESULT IXAudio2SubmixVoice::SetEffectParameters(UINT32, const void* parameters, UINT32 size, UINT32)
{
    if (!parameters || size != sizeof(XAUDIO2FX_REVERB_PARAMETERS)) return E_INVALIDARG;
    const auto& settings = *static_cast<const XAUDIO2FX_REVERB_PARAMETERS*>(parameters);
    EngineLock lock(engine);
    damping = std::clamp(1.0f - settings.DecayHFRatio, 0.05f, 0.95f);
    for (Delay& d : delays)
        d.feedback = std::pow(0.001f, float(d.data.size()) / (float(kOutputRate) * std::max(0.1f, settings.DecayTime)));
    return S_OK;
}
HRESULT IXAudio2SubmixVoice::EnableEffect(UINT32, UINT32) { EngineLock lock(engine); effect = true; return S_OK; }
HRESULT IXAudio2SubmixVoice::DisableEffect(UINT32, UINT32) { EngineLock lock(engine); effect = false; return S_OK; }
void IXAudio2SubmixVoice::Reverb(float left, float right, float& outLeft, float& outRight)
{
    const float in = (left + right) * 0.125f;
    float sums[2] = {};
    for (size_t i = 0; i < delays.size(); ++i)
    {
        Delay& d = delays[i];
        const float tail = d.data[d.position];
        d.filtered = tail * (1.0f - damping) + d.filtered * damping;
        d.data[d.position] = in + d.filtered * d.feedback;
        if (++d.position == d.data.size()) d.position = 0;
        sums[i / 4] += tail;
    }
    outLeft += sums[0];
    outRight += sums[1];
}
void IXAudio2SubmixVoice::DestroyVoice()
{
    EngineLock lock(engine);
    // (the voices sending here lose that send)
    auto unlink = [this](IXAudio2Voice* v) {
        std::lock_guard<std::mutex> levels(v->levelsLock);
        for (unsigned i = v->sendCount; i-- > 0;)
            if (v->sends[i].destination == this)
            {
                for (unsigned j = i; j + 1 < v->sendCount; ++j) v->sends[j] = v->sends[j + 1];
                --v->sendCount;
            }
    };
    for (IXAudio2SourceVoice* v : engine->voices) unlink(v);
    for (IXAudio2SubmixVoice* v : engine->submixes) unlink(v);
    std::erase(engine->submixes, this);
    dead = true;
    if (engine->mixing) engine->graveyard.push_back(this);
    else delete this;
}

IXAudio2SourceVoice::IXAudio2SourceVoice(IXAudio2* owner, const WAVEFORMATEX& wave, float maxFrequencyRatio)
    : IXAudio2Voice(owner, wave.nChannels, wave.nSamplesPerSec), format(wave), maxRatio(maxFrequencyRatio)
{
}
HRESULT IXAudio2SourceVoice::Start(UINT32, UINT32) { EngineLock lock(engine); playing = true; return S_OK; }
HRESULT IXAudio2SourceVoice::Stop(UINT32, UINT32) { EngineLock lock(engine); playing = false; return S_OK; }
HRESULT IXAudio2SourceVoice::FlushSourceBuffers()
{
    EngineLock lock(engine);
    queue.clear();
    position = played = 0;
    loopsLeft = 0;
    bufferStarted = false;
    return S_OK;
}
HRESULT IXAudio2SourceVoice::SubmitSourceBuffer(const XAUDIO2_BUFFER* buffer, const void*)
{
    if (!buffer || !buffer->pAudioData || !buffer->AudioBytes || buffer->AudioBytes % format.nBlockAlign) return E_INVALIDARG;
    const UINT32 total = buffer->AudioBytes / format.nBlockAlign;
    const UINT32 end = buffer->PlayLength ? buffer->PlayBegin + buffer->PlayLength : total;
    if (buffer->PlayBegin >= total || end > total || end <= buffer->PlayBegin) return E_INVALIDARG;
    if (buffer->LoopCount)
    {
        const UINT32 loopEnd = buffer->LoopLength ? buffer->LoopBegin + buffer->LoopLength : end;
        if (buffer->LoopBegin >= end || loopEnd > end || loopEnd <= buffer->LoopBegin) return E_INVALIDARG;
    }
    EngineLock lock(engine);
    if (queue.empty())
    {
        position = uint64_t(buffer->PlayBegin) << 32;
        loopsLeft = buffer->LoopCount;
        bufferStarted = false;
    }
    queue.push_back(*buffer);
    return S_OK;
}
HRESULT IXAudio2SourceVoice::SetFrequencyRatio(float value, UINT32)
{
    if (!std::isfinite(value)) return E_INVALIDARG;
    std::lock_guard<std::mutex> levels(levelsLock);
    ratio = std::clamp(value, XAUDIO2_MIN_FREQ_RATIO, maxRatio);
    return S_OK;
}
void IXAudio2SourceVoice::GetState(XAUDIO2_VOICE_STATE* state, UINT32)
{
    if (!state) return;
    EngineLock lock(engine);
    state->BuffersQueued = UINT32(queue.size());
    state->SamplesPlayed = played >> 32;
    state->pCurrentBufferContext = queue.empty() ? nullptr : queue.front().pContext;
}
void IXAudio2SourceVoice::DestroyVoice()
{
    EngineLock lock(engine);
    std::erase(engine->voices, this);
    dead = true;
    if (engine->mixing) engine->graveyard.push_back(this);
    else delete this;
}

namespace
{
// The front buffer's loops and end: false when the queue ran dry (or a callback destroyed the voice).
bool Advance(IXAudio2SourceVoice& v)
{
    while (!v.queue.empty())
    {
        const XAUDIO2_BUFFER& b = v.queue.front();
        const Region r = RegionOf(b, v.format.nBlockAlign);
        uint64_t frame = v.position >> 32;
        if (v.loopsLeft && frame >= r.loopEnd)
        {
            const uint64_t length = r.loopEnd - r.loopBegin;
            uint64_t wraps = (frame - r.loopBegin) / length;
            if (v.loopsLeft != XAUDIO2_LOOP_INFINITE) wraps = std::min<uint64_t>(wraps, v.loopsLeft);
            v.position -= (wraps * length) << 32;
            if (v.loopsLeft != XAUDIO2_LOOP_INFINITE) v.loopsLeft -= UINT32(wraps);
            if (v.callback) v.callback->OnLoopEnd(b.pContext);
            if (v.dead) return false;
            frame = v.position >> 32;
        }
        if (frame < r.end) return true;
        // done: the next buffer starts where this one's overshoot leaves off
        const uint64_t overshoot = v.position - (uint64_t(r.end) << 32);
        const XAUDIO2_BUFFER done = b;
        v.queue.pop_front();
        v.bufferStarted = false;
        if (v.callback)
        {
            v.callback->OnBufferEnd(done.pContext);
            if (v.dead) return false;
            if (done.Flags & XAUDIO2_END_OF_STREAM) v.callback->OnStreamEnd();
            if (v.dead) return false;
        }
        if (v.queue.empty())
        {
            v.position = 0;
            return false;
        }
        v.position = (uint64_t(v.queue.front().PlayBegin) << 32) + overshoot;
        v.loopsLeft = v.queue.front().LoopCount;
    }
    return false;
}

template <typename Reader>
void MixVoice(IXAudio2SourceVoice& v, const IXAudio2Voice::Levels& levels, float ratio, float* master, unsigned frames)
{
    const uint64_t step = uint64_t(double(v.sampleRate) * double(ratio) / double(kOutputRate) * kFixed + 0.5);
    const UINT32 channels = std::min<UINT32>(v.channels, 2);
    for (unsigned f = 0; f < frames; ++f)
    {
        if (!Advance(v)) return;
        const XAUDIO2_BUFFER& b = v.queue.front();
        if (!v.bufferStarted)
        {
            v.bufferStarted = true;
            if (v.callback) v.callback->OnBufferStart(b.pContext);
            if (v.dead || v.queue.empty()) return;
        }
        const XAUDIO2_BUFFER& cur = v.queue.front();
        const Region r = RegionOf(cur, v.format.nBlockAlign);
        const UINT32 i0 = UINT32(v.position >> 32);
        UINT32 i1 = i0 + 1;
        if (v.loopsLeft && i1 >= r.loopEnd) i1 = r.loopBegin;
        else if (i1 >= r.end) i1 = r.end - 1;
        const float t = float(uint32_t(v.position)) * float(1.0 / kFixed);
        float s[2] = {0.0f, 0.0f};
        for (UINT32 c = 0; c < channels; ++c)
        {
            const float a = Reader::Read(v.format, cur.pAudioData, i0, c);
            const float n = Reader::Read(v.format, cur.pAudioData, i1, c);
            s[c] = (a + (n - a) * t) * levels.volume;
        }
        v.Route(levels, s[0], s[1], f, master);
        v.position += step;
        v.played += step;
    }
    Advance(v);
}

// The bytes the voice lacks for a pass of `frames` (OnVoiceProcessingPassStart's argument).
UINT32 BytesRequired(const IXAudio2SourceVoice& v, unsigned frames)
{
    if (!v.playing) return 0;
    float ratio;
    {
        std::lock_guard<std::mutex> levels(v.levelsLock);
        ratio = v.ratio;
    }
    const double need = double(frames) * double(v.sampleRate) * double(ratio) / double(kOutputRate);
    double have = 0.0;
    bool first = true;
    for (const XAUDIO2_BUFFER& b : v.queue)
    {
        const Region r = RegionOf(b, v.format.nBlockAlign);
        const double start = first ? double(v.position) / kFixed : double(b.PlayBegin);
        if (b.LoopCount == XAUDIO2_LOOP_INFINITE) return 0;
        have += std::max(0.0, double(r.end) - start) + double(b.LoopCount) * double(r.loopEnd - r.loopBegin);
        first = false;
        if (have >= need) return 0;
    }
    return UINT32(std::ceil(need - have)) * v.format.nBlockAlign;
}
}  // namespace

void IXAudio2SourceVoice::Mix(float* master, unsigned frames)
{
    if (!playing || queue.empty() || dead) return;
    Levels levels;
    float r;
    {
        std::lock_guard<std::mutex> lock(levelsLock);
        levels = {volume, sends, sendCount};
        r = ratio;
    }
    if (format.wFormatTag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16) MixVoice<Pcm16>(*this, levels, r, master, frames);
    else MixVoice<AnyPcm>(*this, levels, r, master, frames);
}

// ------------------------------------------------------------------------------------------------ the engine
HRESULT IXAudio2::CreateMasteringVoice(IXAudio2MasteringVoice** out, UINT32 channels, UINT32 rate, UINT32, UINT32, const void*, int)
{
    if (!out) return E_POINTER;
    if ((channels && channels != 2) || (rate && rate != kOutputRate)) return E_INVALIDARG;
    EngineLock lock(this);
    if (master) return E_INVALIDARG;
    *out = master = new IXAudio2MasteringVoice(this, 2, kOutputRate);
    return S_OK;
}
HRESULT IXAudio2::CreateSubmixVoice(IXAudio2SubmixVoice** out, UINT32 channels, UINT32 rate, UINT32, UINT32 stage,
                                    const XAUDIO2_VOICE_SENDS* outputs, const XAUDIO2_EFFECT_CHAIN* chain)
{
    if (!out) return E_POINTER;
    if (channels != 2 || (rate && rate != kOutputRate) ||
        (chain && (chain->EffectCount > 1 || (chain->EffectCount && !chain->pEffectDescriptors))) ||
        (outputs && (outputs->SendCount > 4 || (outputs->SendCount && !outputs->pSends))))
        return E_INVALIDARG;
    EngineLock lock(this);
    // (a submix only sends to the master or to a later stage)
    if (outputs)
        for (unsigned i = 0; i < outputs->SendCount; ++i)
        {
            IXAudio2Voice* d = outputs->pSends[i].pOutputVoice;
            if (d != master && (!KnownDestination(this, d) || static_cast<IXAudio2SubmixVoice*>(d)->stage <= stage))
                return E_INVALIDARG;
        }
    auto* voice = new IXAudio2SubmixVoice(this, channels, kOutputRate);
    voice->stage = stage;
    voice->effect = chain && chain->EffectCount && chain->pEffectDescriptors[0].InitialState;
    const HRESULT r = voice->SetOutputVoices(outputs);
    if (FAILED(r)) { delete voice; return r; }
    submixes.push_back(voice);
    std::stable_sort(submixes.begin(), submixes.end(), [](const IXAudio2SubmixVoice* a, const IXAudio2SubmixVoice* b) { return a->stage < b->stage; });
    *out = voice;
    return S_OK;
}
HRESULT IXAudio2::CreateSourceVoice(IXAudio2SourceVoice** out, const WAVEFORMATEX* wave, UINT32, float maxRatio,
                                    IXAudio2VoiceCallback* callback, const XAUDIO2_VOICE_SENDS* outputs, const XAUDIO2_EFFECT_CHAIN*)
{
    if (!out || !wave) return E_POINTER;
    const bool isFloat = wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (wave->nChannels < 1 || wave->nChannels > 2 || wave->nSamplesPerSec < XAUDIO2_MIN_SAMPLE_RATE ||
        wave->nSamplesPerSec > XAUDIO2_MAX_SAMPLE_RATE || !std::isfinite(maxRatio) || maxRatio < 1.0f ||
        maxRatio > XAUDIO2_MAX_FREQ_RATIO || (wave->wFormatTag != WAVE_FORMAT_PCM && !isFloat) ||
        (wave->wBitsPerSample != 8 && wave->wBitsPerSample != 16 && wave->wBitsPerSample != 24 && wave->wBitsPerSample != 32) ||
        (isFloat && wave->wBitsPerSample != 32) || wave->nBlockAlign != wave->nChannels * (wave->wBitsPerSample / 8))
        return E_INVALIDARG;
    EngineLock lock(this);
    auto* voice = new IXAudio2SourceVoice(this, *wave, maxRatio);
    voice->callback = callback;
    const HRESULT r = voice->SetOutputVoices(outputs);
    if (FAILED(r)) { delete voice; return r; }
    voices.push_back(voice);
    *out = voice;
    return S_OK;
}
HRESULT IXAudio2::RegisterForCallbacks(IXAudio2EngineCallback* callback)
{
    if (!callback) return E_POINTER;
    EngineLock lock(this);
    if (std::find(callbacks.begin(), callbacks.end(), callback) == callbacks.end()) callbacks.push_back(callback);
    return S_OK;
}
void IXAudio2::UnregisterForCallbacks(IXAudio2EngineCallback* callback)
{
    EngineLock lock(this);
    std::erase(callbacks, callback);
}
void IXAudio2::NotifyCriticalError(HRESULT error)
{
    EngineLock lock(this);
    for (IXAudio2EngineCallback* c : callbacks) c->OnCriticalError(error);
}
void IXAudio2::Lock() { if (native) rmutexLock(&static_cast<Device*>(native)->lock); }
void IXAudio2::Unlock() { if (native) rmutexUnlock(&static_cast<Device*>(native)->lock); }

void IXAudio2::Mix(float* output, unsigned frames)
{
    std::fill_n(output, size_t(frames) * 2, 0.0f);
    mixing = true;
    for (IXAudio2SubmixVoice* s : submixes) std::fill_n(s->input.data(), std::min<size_t>(s->input.size(), size_t(frames) * 2), 0.0f);
    for (IXAudio2EngineCallback* c : callbacks) c->OnProcessingPassStart();
    g_passVoices.assign(voices.begin(), voices.end());
    for (IXAudio2SourceVoice* v : g_passVoices)
    {
        if (v->dead) continue;
        if (v->callback) v->callback->OnVoiceProcessingPassStart(BytesRequired(*v, frames));
        if (!v->dead) v->Mix(output, frames);
        if (!v->dead && v->callback) v->callback->OnVoiceProcessingPassEnd();
    }
    g_passSubmixes.assign(submixes.begin(), submixes.end());
    for (IXAudio2SubmixVoice* s : g_passSubmixes)
    {
        if (s->dead) continue;
        const IXAudio2Voice::Levels levels = s->CurrentLevels();
        for (unsigned f = 0; f < frames; ++f)
        {
            float l = 0.0f, r = 0.0f;
            const float inL = s->input[size_t(f) * 2], inR = s->input[size_t(f) * 2 + 1];
            if (s->effect) s->Reverb(inL, inR, l, r);
            else { l = inL; r = inR; }
            s->Route(levels, l * levels.volume, r * levels.volume, f, output);
        }
    }
    const float gain = master ? master->CurrentLevels().volume : 0.0f;
    for (size_t i = 0; i < size_t(frames) * 2; ++i) output[i] = std::clamp(output[i] * gain, -1.0f, 1.0f);
    for (IXAudio2EngineCallback* c : callbacks) c->OnProcessingPassEnd();
    mixing = false;
    for (IXAudio2Voice* v : graveyard) delete v;
    graveyard.clear();
}

// ------------------------------------------------------------------------------------------------ the output
namespace
{
Result FillAndQueue(Device& d, AudioOutBuffer& buffer)
{
    rmutexLock(&d.lock);
    d.engine->Mix(d.mix.data(), kFrames);
    rmutexUnlock(&d.lock);
    auto* out = static_cast<int16_t*>(buffer.buffer);
    for (size_t i = 0; i < d.mix.size(); ++i) out[i] = ToPcm16(d.mix[i]);
    armDCacheFlush(buffer.buffer, buffer.data_size);
    return audoutAppendAudioOutBuffer(&buffer);
}

void OutputThread(void* arg)
{
    Device& d = *static_cast<Device*>(arg);
    while (!d.stop.load(std::memory_order_acquire))
    {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        Result rc = audoutWaitPlayFinish(&released, &count, 100'000'000ull);
        if (d.stop.load(std::memory_order_acquire)) break;
        if (R_FAILED(rc) && rc != KERNELRESULT(TimedOut)) { d.error = rc; break; }
        if (!count) continue;
        if (count == 1 && released) rc = FillAndQueue(d, *released);
        else
            for (AudioOutBuffer& b : d.buffers)
            {
                bool queued = false;
                rc = audoutContainsAudioOutBuffer(&b, &queued);
                if (R_SUCCEEDED(rc) && !queued) rc = FillAndQueue(d, b);
                if (R_FAILED(rc)) break;
            }
        if (R_FAILED(rc)) { d.error = rc; break; }
    }
    if (R_FAILED(d.error.load())) d.engine->NotifyCriticalError(E_FAIL);
}

Device* g_output = nullptr;   // (the running engine, for CaneSwitch::StopAudio)

// (buffers left queued to audout at exit stay locked: the Homebrew loader cannot take that memory back)
void StopOutput(Device& d)
{
    d.stop.store(true, std::memory_order_release);
    if (d.threadStarted) threadWaitForExit(&d.thread);
    if (d.threadCreated) threadClose(&d.thread);
    d.threadStarted = d.threadCreated = false;
    if (d.service)
    {
        audoutStopAudioOut();
        bool flushed = false;
        audoutFlushAudioOutBuffers(&flushed);
        audoutExit();
        d.service = false;
    }
    for (AudioOutBuffer& b : d.buffers)
    {
        std::free(b.buffer);
        b.buffer = nullptr;
    }
}
}  // namespace

void CaneSwitch::StopAudio()
{
    if (g_output) StopOutput(*g_output);
}

void IXAudio2::Release()
{
    if (auto* d = static_cast<Device*>(native))
    {
        StopOutput(*d);
        if (g_output == d) g_output = nullptr;
        delete d;
        native = nullptr;
    }
    for (IXAudio2SourceVoice* v : voices) delete v;
    for (IXAudio2SubmixVoice* s : submixes) delete s;
    for (IXAudio2Voice* v : graveyard) delete v;
    delete master;
    delete this;
}

HRESULT XAudio2Create(IXAudio2** out, UINT32, UINT32)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    auto* engine = new IXAudio2;
    auto* d = new Device;
    d->engine = engine;
    rmutexInit(&d->lock);
    engine->native = d;
    Result rc = audoutInitialize();
    d->service = R_SUCCEEDED(rc);
    if (R_FAILED(rc) || audoutGetSampleRate() != kOutputRate || audoutGetChannelCount() != 2 || audoutGetPcmFormat() != PcmFormat_Int16)
    {
        engine->Release();
        return E_FAIL;
    }
    for (AudioOutBuffer& b : d->buffers)
    {
        b.buffer = std::aligned_alloc(0x1000, 0x1000);
        b.buffer_size = 0x1000;
        b.data_size = kFrames * 2 * sizeof(int16_t);
        if (!b.buffer) { engine->Release(); return E_FAIL; }
        if (R_FAILED(rc = FillAndQueue(*d, b))) { engine->Release(); return E_FAIL; }
    }
    rc = audoutStartAudioOut();
    if (R_SUCCEEDED(rc))
    {
        rc = threadCreate(&d->thread, OutputThread, d, nullptr, 0x10000, 0x2B, CaneSwitch::MixerCore());
        d->threadCreated = R_SUCCEEDED(rc);
        if (R_SUCCEEDED(rc)) d->threadStarted = R_SUCCEEDED(rc = threadStart(&d->thread));
    }
    if (R_FAILED(rc))
    {
        engine->Release();
        return E_FAIL;
    }
    g_output = d;
    *out = engine;
    return S_OK;
}
