#include "audio.h"

// FAudio on every platform (on Windows too, over SDL2), so the mixer behaves the same everywhere.
#ifdef __SWITCH__
#include "../platform/switch/audio_switch.h"   // (the Switch's own mixer, with the same surface)
#else
#include "../platform/audio_linux.h"
#include <SDL.h>   // (the devices by name, the microphone: notes/AUDIO_2026-11-10.md)
#define SLY2_AUDIO_DEVICES 1
#endif
#include "../platform/audio_devices.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "../platform/platform.h"

IXAudio2* g_pxaudio2 = nullptr;
IXAudio2MasteringVoice* g_pmastervoice = nullptr;
std::recursive_mutex g_mtxSound;

namespace
{
bool s_fAudioFailed = false;
float s_gOutput = 1.0f;
bool s_fMute = false;
bool s_fMono = false;
// (PC) the bus every game voice feeds (a stereo 48 kHz submix into the mastering voice, unity: the same mix): the
// mastering voice, which holds the device, can then be replaced (another device) while the voices keep playing
IXAudio2SubmixVoice* s_pbus = nullptr;
cane::audio::DeviceFollower s_devOut, s_devIn, s_devHeadset;
// (the USB headset) its speaker: a second engine on the Headset Output Device, the dialogue lines' streams in it
IXAudio2* s_pxaudio2Headset = nullptr;
IXAudio2MasteringVoice* s_pmasterHeadset = nullptr;
int s_cHeadsetStreams = 0;
bool s_fMicWanted = false;

void ApplyMasterVolume()
{
    if (g_pmastervoice) g_pmastervoice->SetVolume(s_fMute ? 0.0f : s_gOutput * kOutputGain);
}

#ifdef SLY2_AUDIO_DEVICES
// SDL's devices (SDL_INIT_AUDIO taken once: FAudio takes its own reference too)
bool FSdlAudio()
{
    static const bool s_f = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
    return s_f;
}
// (--audio-selftest: the devices SDL lists replaced, to unplug / plug a device)
std::vector<std::string> (*s_pfnListHook)(bool fCapture) = nullptr;
std::vector<std::string> AszDevices(bool fCapture)
{
    if (s_pfnListHook) return s_pfnListHook(fCapture);
    std::vector<std::string> v;
    if (!FSdlAudio()) return v;
    const int c = SDL_GetNumAudioDevices(fCapture ? 1 : 0);
    for (int i = 0; i < c; ++i)
        if (const char* sz = SDL_GetAudioDeviceName(i, fCapture ? 1 : 0)) v.push_back(cane::audio::cleanName(sz));
    return v;
}
// FAudio's device index of a name: SDL's index + 1 (FAudio_platform_sdl2.c opens SDL_GetAudioDeviceName(index - 1));
// 0 the default
uint32_t IdeviceOutput(const std::string& name)
{
    if (name.empty()) return 0;
    const int i = cane::audio::findDevice(AszDevices(false), name);
    return i < 0 ? 0 : uint32_t(i + 1);
}
// The device opens (probed with FAudio's spec, closed again): a device listed but gone does not leave the game
// without a mastering voice
bool FProbeOutput(const std::string& name)
{
    if (!FSdlAudio()) return false;
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 480;
    const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(name.empty() ? nullptr : name.c_str(), 0, &want, &have, 0);
    if (!dev) return false;
    SDL_CloseAudioDevice(dev);
    return true;
}
#endif

// A mastering voice on the device `name` ("" the default); FAudio's device index resolved by name, the default when
// it cannot open
bool FCreateMaster(IXAudio2* px, IXAudio2MasteringVoice** ppm, const std::string& name, std::string* popened)
{
#ifdef SLY2_AUDIO_DEVICES
    if (const uint32_t idev = IdeviceOutput(name); idev && SUCCEEDED(px->CreateMasteringVoice(ppm, 2, 48000, idev)) && *ppm)
    {
        if (popened) *popened = name;
        return true;
    }
    if (!name.empty()) std::fprintf(stderr, "audio: output device \"%s\" not available: the default\n", name.c_str());
#else
    (void)name;
#endif
    *ppm = nullptr;
    if (FAILED(px->CreateMasteringVoice(ppm)) || !*ppm)
    {
        *ppm = nullptr;
        return false;
    }
    if (popened) popened->clear();
    return true;
}
}  // namespace

bool FEnsureAudio()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (g_pxaudio2) return true;
    if (s_fAudioFailed) return false;
    if (FAILED(XAudio2Create(&g_pxaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR)))
    {
        g_pxaudio2 = nullptr;
        s_fAudioFailed = true;
        std::fprintf(stderr, "audio: no audio engine\n");
        return false;
    }
    std::string opened;
    if (!FCreateMaster(g_pxaudio2, &g_pmastervoice, s_devOut.wanted, &opened))
    {
        g_pxaudio2->Release();
        g_pxaudio2 = nullptr;
        g_pmastervoice = nullptr;
        s_fAudioFailed = true;
        std::fprintf(stderr, "audio: no audio device\n");
        return false;
    }
    s_devOut.open = opened;
    s_devOut.started = true;
#ifdef SLY2_AUDIO_DEVICES
    if (FAILED(g_pxaudio2->CreateSubmixVoice(&s_pbus, 2, 48000, 0, 0, nullptr, nullptr))) s_pbus = nullptr;
#endif
    g_pmastervoice->SetVolume(kOutputGain);
    return true;
}

IXAudio2Voice* PvoiceOutput() { return s_pbus ? static_cast<IXAudio2Voice*>(s_pbus) : static_cast<IXAudio2Voice*>(g_pmastervoice); }

namespace
{
WAVEFORMATEX FormatPcm16(int cch, int rate)
{
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<uint16_t>(cch);
    format.nSamplesPerSec = static_cast<uint32_t>(rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<uint16_t>(2 * cch);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}
}  // namespace

bool FCreateGameSourceVoice(IXAudio2SourceVoice** ppvoice, int cch, int rate, float frqMax)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    *ppvoice = nullptr;
    if (!FEnsureAudio()) return false;
    const WAVEFORMATEX format = FormatPcm16(cch, rate);
    HRESULT hr = E_FAIL;
    if (!s_pbus) hr = g_pxaudio2->CreateSourceVoice(ppvoice, &format, 0, frqMax);
#ifdef SLY2_AUDIO_DEVICES
    else
    {
        XAUDIO2_SEND_DESCRIPTOR send{0, s_pbus};
        XAUDIO2_VOICE_SENDS sends{1, &send};
        hr = g_pxaudio2->CreateSourceVoice(ppvoice, &format, 0, frqMax, nullptr, &sends);
    }
#endif
    if (FAILED(hr) || !*ppvoice)
    {
        *ppvoice = nullptr;
        return false;
    }
    return true;
}

void SetOutputVolume(float g)
{
    s_gOutput = std::clamp(g, 0.0f, 1.0f);
    if (FEnsureAudio()) ApplyMasterVolume();
}
void SetOutputMute(bool fMute)
{
    if (s_fMute == fMute) return;
    s_fMute = fMute;
    ApplyMasterVolume();
}

namespace
{
void CloseHeadsetOutput()
{
    if (s_pmasterHeadset) s_pmasterHeadset->DestroyVoice();
    s_pmasterHeadset = nullptr;
    if (s_pxaudio2Headset) s_pxaudio2Headset->Release();
    s_pxaudio2Headset = nullptr;
    s_devHeadset.open.clear();
    s_devHeadset.started = false;
}
void CloseMic();
}  // namespace

void ShutdownAudio()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    CloseMic();
    CloseHeadsetOutput();
    if (s_pbus) s_pbus->DestroyVoice();
    s_pbus = nullptr;
    if (g_pmastervoice) g_pmastervoice->DestroyVoice();
    g_pmastervoice = nullptr;
    if (g_pxaudio2) g_pxaudio2->Release();
    g_pxaudio2 = nullptr;
}

void SetMonoOutput(bool fMono) { s_fMono = fMono; }

void SetVoiceLevels(IXAudio2SourceVoice* pvoice, int cchSource, float gLeft, float gRight)
{
    IXAudio2Voice* const pout = PvoiceOutput();
    if (!pvoice || !pout) return;
    // The mastering voice (and the bus) is stereo (audio_linux.h opens 2 channels at 48 kHz).
    if (s_fMono)
    {
        // (Speakers: Mono) both outputs carry the source's channels at the mean of the two levels
        const float g = (gLeft + gRight) * 0.5f;
        if (cchSource == 1)
        {
            const float matrix[2] = {g, g};
            pvoice->SetOutputMatrix(pout, 1, 2, matrix);
        }
        else
        {
            const float matrix[4] = {g * 0.5f, g * 0.5f, g * 0.5f, g * 0.5f};
            pvoice->SetOutputMatrix(pout, 2, 2, matrix);
        }
        return;
    }
    if (cchSource == 1)
    {
        const float matrix[2] = {gLeft, gRight};
        pvoice->SetOutputMatrix(pout, 1, 2, matrix);
    }
    else
    {
        // Row-major [destination][source]: left from the left channel, right from the right one.
        const float matrix[4] = {gLeft, 0.0f, 0.0f, gRight};
        pvoice->SetOutputMatrix(pout, 2, 2, matrix);
    }
}

// ------------------------------------------------------------ (PC) the devices (notes/AUDIO_2026-11-10.md)
namespace
{
// The game's output onto `name` ("" the default): the bus let go of the old mastering voice, a new one made on the
// device, the bus routed to it (the voices keep playing; FAudio's update size and rate stay 480 / 48 kHz: SDL
// converts, so the voices' buffers fit the new device)
void ReopenOutput(const std::string& name)
{
#ifdef SLY2_AUDIO_DEVICES
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (!g_pxaudio2 || !s_pbus) return;
    std::string target = name;
    if (!FProbeOutput(target))
    {
        if (target.empty() || !FProbeOutput(std::string())) return;   // (nothing opens: tried again at the next poll)
        target.clear();
    }
    XAUDIO2_VOICE_SENDS none{0, nullptr};
    s_pbus->SetOutputVoices(&none);
    if (g_pmastervoice) g_pmastervoice->DestroyVoice();
    g_pmastervoice = nullptr;
    std::string opened;
    if (!FCreateMaster(g_pxaudio2, &g_pmastervoice, target, &opened))
    {
        std::fprintf(stderr, "audio: no audio device\n");
        s_devOut.started = false;   // (tried again at the next poll)
        return;
    }
    s_devOut.open = opened;
    s_pbus->SetOutputVoices(nullptr);   // (the default: the new mastering voice)
    ApplyMasterVolume();
    std::printf("audio: output on %s\n", opened.empty() ? "the system default" : ("\"" + opened + "\"").c_str());
#else
    (void)name;
#endif
}

#ifdef SLY2_AUDIO_DEVICES
// ---- the microphone (orig: (IOP) spiop.irx's lgaud thread 0x10CC: the one USB audio device open for recording, 16-bit
// mono (11025..22050 Hz), 0x800 bytes read at a time; each read's level = sqrt(the mean of its 1024 samples' squares)
// / 32768 in single floats (0x11B4..0x1210), kept at +0x20, which 'SUKR' 11 returns; a failed read: closed, level 0)
SDL_AudioDeviceID s_devMic = 0;
std::atomic<float> s_gMic{0.0f};
float s_gMicSum = 0.0f;
int s_cMicSample = 0;
constexpr int kcMicWindow = 1024;
void AddMicSamples(const int16_t* as, int c)
{
    for (int i = 0; i < c; ++i)
    {
        const int s = as[i];
        s_gMicSum = s_gMicSum + static_cast<float>(s * s);   // (0x11C8 mult, 0x38E0 int -> float, 0x3078 add)
        if (++s_cMicSample == kcMicWindow)
        {
            // (0x3134 x 1/1024 (0x3A800000), 0x50F0 sqrt, x 1/32768 (0x38000000))
            const float gMean = s_gMicSum * (1.0f / 1024.0f);
            s_gMic.store(std::sqrt(gMean) * (1.0f / 32768.0f), std::memory_order_relaxed);
            s_gMicSum = 0.0f;
            s_cMicSample = 0;
        }
    }
}
void SDLCALL MicCallback(void*, Uint8* pb, int cb) { AddMicSamples(reinterpret_cast<const int16_t*>(pb), cb / 2); }
void OpenMic(const std::string& name)
{
    if (!FSdlAudio()) return;
    SDL_AudioSpec want{}, have{};
    want.freq = 22050;   // (the IRX's lgaud format: 11025..22050 Hz; the level does not depend on it)
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = kcMicWindow;
    want.callback = MicCallback;
    s_gMicSum = 0.0f;
    s_cMicSample = 0;
    s_devMic = SDL_OpenAudioDevice(name.empty() ? nullptr : name.c_str(), 1, &want, &have, 0);
    if (!s_devMic && !name.empty())
    {
        std::fprintf(stderr, "audio: microphone \"%s\" not available: the default\n", name.c_str());
        s_devMic = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0);
        s_devIn.open.clear();
    }
    static bool s_fReported = false;
    if (!s_devMic)
    {
        if (!s_fReported) std::fprintf(stderr, "audio: no microphone (%s)\n", SDL_GetError());
        s_fReported = true;
        return;
    }
    s_fReported = false;
    SDL_PauseAudioDevice(s_devMic, 0);
    std::printf("audio: microphone %s open (USB Headset)\n", s_devIn.open.empty() ? "(system default)" : ("\"" + s_devIn.open + "\"").c_str());
}
void CloseMic()
{
    if (s_devMic) SDL_CloseAudioDevice(s_devMic);
    s_devMic = 0;
    s_gMic.store(0.0f, std::memory_order_relaxed);
    s_devIn.started = false;
    s_devIn.open.clear();
}
#else
void CloseMic() {}
#endif

// The headset's speaker: open on the Headset Output Device while USB Headset is on, when one is chosen, present and not
// the game's own output (then the lines play as without a headset)
void FollowHeadsetOutput(const std::vector<std::string>& outputs)
{
#ifdef SLY2_AUDIO_DEVICES
    const bool fWanted = s_fMicWanted && !s_devHeadset.wanted.empty();
    const int i = fWanted ? cane::audio::findDevice(outputs, s_devHeadset.wanted) : -1;
    const std::string target = i >= 0 ? outputs[size_t(i)] : std::string();
    const std::string want = !target.empty() && target == s_devOut.open ? std::string() : target;
    if (s_pxaudio2Headset && s_devHeadset.open == want) return;
    if (!s_pxaudio2Headset && want.empty()) return;
    if (s_cHeadsetStreams > 0) return;   // (a line playing there finishes first: SDL keeps a lost device's clock)
    CloseHeadsetOutput();
    if (want.empty()) return;
    if (FAILED(XAudio2Create(&s_pxaudio2Headset, 0, XAUDIO2_DEFAULT_PROCESSOR)))
    {
        s_pxaudio2Headset = nullptr;
        return;
    }
    std::string opened;
    if (!FCreateMaster(s_pxaudio2Headset, &s_pmasterHeadset, want, &opened) || opened != want)
    {
        CloseHeadsetOutput();
        return;
    }
    s_pmasterHeadset->SetVolume(kOutputGain);
    s_devHeadset.open = opened;
    s_devHeadset.started = true;
    std::printf("audio: USB Headset dialogue on \"%s\"\n", opened.c_str());
#else
    (void)outputs;
#endif
}
}  // namespace

void SetAudioDevices(const std::string& output, const std::string& input, const std::string& headsetOutput)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const std::string out = cane::audio::cleanName(output), in = cane::audio::cleanName(input);
    const bool fChanged = out != s_devOut.wanted || in != s_devIn.wanted || cane::audio::cleanName(headsetOutput) != s_devHeadset.wanted;
    s_devOut.wanted = out;
    if (in != s_devIn.wanted)
    {
        s_devIn.wanted = in;
        CloseMic();   // (reopened on the new one by the poll, while USB Headset is on)
    }
    s_devHeadset.wanted = cane::audio::cleanName(headsetOutput);
    if (fChanged && g_pxaudio2) PollAudioDevices(true);
}

void SetMicCapture(bool fOn)
{
    if (fOn == s_fMicWanted) return;
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    s_fMicWanted = fOn;
    if (!fOn) CloseMic();
    PollAudioDevices(true);
}

float GMicLevel()
{
#ifdef SLY2_AUDIO_DEVICES
    return s_devMic ? s_gMic.load(std::memory_order_relaxed) : 0.0f;
#else
    return 0.0f;
#endif
}

void PollAudioDevices(bool fNow)
{
#ifdef SLY2_AUDIO_DEVICES
    // (every device the default and no headset: no work at all, the default device as before)
    if (!g_pxaudio2 || (s_devOut.wanted.empty() && s_devOut.open.empty() && !s_fMicWanted && !s_pxaudio2Headset)) return;
    static auto s_tNext = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (!fNow && now < s_tNext) return;
    s_tNext = now + std::chrono::seconds(2);
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const std::vector<std::string> outputs = AszDevices(false);
    // the game's output: the chosen device while it is present, else the default; taken again when it is back
    cane::audio::DeviceFollower f = s_devOut;
    if (f.step(outputs) && f.open != s_devOut.open) ReopenOutput(f.open);
    // the microphone (only while USB Headset is on)
    if (s_fMicWanted)
    {
        const std::vector<std::string> inputs = AszDevices(true);
        if (s_devIn.step(inputs) || !s_devMic)
        {
            if (s_devMic) SDL_CloseAudioDevice(s_devMic);
            s_devMic = 0;
            s_devIn.started = true;
            OpenMic(s_devIn.open);
        }
    }
    FollowHeadsetOutput(outputs);
#else
    (void)fNow;
#endif
}

bool FAudioDeviceChoiceShown()
{
#if defined(SLY2_AUDIO_DEVICES) && !defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

cane::audio::DeviceList ListAudioDevices()
{
    cane::audio::DeviceList out;
    if (cane::audio::fakeDevices(out)) return out;
#ifdef SLY2_AUDIO_DEVICES
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    out.available = FSdlAudio();
    out.outputs = AszDevices(false);
    out.inputs = AszDevices(true);
#endif
    return out;
}

std::string SzAudioOutputOpen() { return s_devOut.open; }
bool FHeadsetOutputReady() { return s_pxaudio2Headset && s_pmasterHeadset; }


bool CPcmStream::FOpen(int cch, int rate, float frqMax, bool fHeadset)
{
    Close();
    if (!FEnsureAudio() || cch < 1 || cch > 2 || rate <= 0) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (fHeadset && !FHeadsetOutputReady()) return false;
    const WAVEFORMATEX format = FormatPcm16(cch, rate);
    const bool fOk = fHeadset ? SUCCEEDED(s_pxaudio2Headset->CreateSourceVoice(&m_pvoice, &format, 0, frqMax)) && m_pvoice
                              : FCreateGameSourceVoice(&m_pvoice, cch, rate, frqMax);
    if (!fOk)
    {
        m_pvoice = nullptr;
        return false;
    }
    m_fHeadset = fHeadset;
    if (fHeadset) ++s_cHeadsetStreams;
    m_cch = cch;
    m_rate = rate;
    m_iab = 0;
    // (test: SLY2N_PCMTAP=dir: the frames every stream is fed, dir/stream-<n>-<ch>ch-<rate>.raw, s16 interleaved; the
    // audio identity check compares these between builds)
    if (static const char* s_szTap = std::getenv("SLY2N_PCMTAP"); s_szTap)
    {
        static int s_cstream = 0;
        char sz[512];
        std::snprintf(sz, sizeof(sz), "%s/stream-%04d-%dch-%d.raw", s_szTap, s_cstream++, cch, rate);
        m_pfTap = std::fopen(sz, "wb");
    }
    SetLevels(1.0f, 1.0f);
    return true;
}

void CPcmStream::Close()
{
    if (m_pfTap) std::fclose(m_pfTap);
    m_pfTap = nullptr;
    if (!m_pvoice) return;
    m_pvoice->Stop();
    m_pvoice->FlushSourceBuffers();
    m_pvoice->DestroyVoice();  // waits for the mixer: the buffers are free after it
    m_pvoice = nullptr;
    if (m_fHeadset) --s_cHeadsetStreams;
    m_fHeadset = false;
}

bool CPcmStream::FPush(const int16_t* asample, std::size_t cframe, bool fEnd)
{
    if (!m_pvoice || CBufferQueued() >= kcBuffer) return false;
    std::vector<int16_t>& ab = m_aab[m_iab];
    m_iab = (m_iab + 1) % kcBuffer;
    ab.assign(asample, asample + cframe * static_cast<std::size_t>(m_cch));
    if (m_pfTap && !ab.empty()) std::fwrite(ab.data(), sizeof(int16_t), ab.size(), m_pfTap);
    if (ab.empty()) ab.assign(static_cast<std::size_t>(m_cch), 0);  // an end marker needs a sample
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(ab.size() * sizeof(int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(ab.data());
    buffer.Flags = fEnd ? XAUDIO2_END_OF_STREAM : 0;
    return SUCCEEDED(m_pvoice->SubmitSourceBuffer(&buffer));
}

int CPcmStream::CBufferQueued() const
{
    if (!m_pvoice) return 0;
    XAUDIO2_VOICE_STATE state{};
    m_pvoice->GetState(&state);
    return static_cast<int>(state.BuffersQueued);
}

uint64_t CPcmStream::CFramePlayed() const
{
    if (!m_pvoice) return 0;
    XAUDIO2_VOICE_STATE state{};
    m_pvoice->GetState(&state);
    return state.SamplesPlayed;
}

void CPcmStream::Start()
{
    if (m_pvoice) m_pvoice->Start();
}

void CPcmStream::Stop()
{
    if (m_pvoice) m_pvoice->Stop();
}

void CPcmStream::SetLevels(float gLeft, float gRight)
{
    if (m_fHeadset)
    {
        const float m[4] = {gLeft, 0.0f, 0.0f, gRight};
        SetMatrix(m);
        return;
    }
    SetVoiceLevels(m_pvoice, m_cch, gLeft, gRight);
}

void CPcmStream::SetMatrix(const float matrix[4])
{
    IXAudio2Voice* const pout = m_fHeadset ? static_cast<IXAudio2Voice*>(s_pmasterHeadset) : PvoiceOutput();
    if (!m_pvoice || !pout) return;
    if (m_cch == 1)
    {
        const float m[2] = {matrix[0], matrix[2]};
        m_pvoice->SetOutputMatrix(pout, 1, 2, m);
    }
    else
        m_pvoice->SetOutputMatrix(pout, 2, 2, matrix);
}

void CPcmStream::SetFrequencyRatio(float r)
{
    if (m_pvoice) m_pvoice->SetFrequencyRatio(std::clamp(r, XAUDIO2_MIN_FREQ_RATIO, 4.0f));
}

// orig: (SPU2 hardware) the ADPCM decoder; Sly1-Release sound.cpp DecodeSpuAdpcm
void DecodeAdpcmBlock(const uint8_t* pb, int16_t* asample, int* ph1, int* ph2)
{
    // The SPU2 takes both header nibbles as 4-bit fields; filters above 4 use the zero tail of its table.
    static constexpr int s_acoef[16][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    const int shift = pb[0] & 0x0F;
    const int filter = (pb[0] >> 4) & 0x0F;
    int h1 = *ph1, h2 = *ph2;
    for (int i = 0; i < 28; ++i)
    {
        int nibble = (i & 1) ? pb[2 + i / 2] >> 4 : pb[2 + i / 2] & 0x0F;
        if (nibble & 8) nibble -= 16;
        int v = (nibble << 12) >> shift;
        v += (s_acoef[filter][0] * h1 + s_acoef[filter][1] * h2 + 32) >> 6;
        v = std::clamp(v, -32768, 32767);
        h2 = h1;
        h1 = v;
        asample[i] = static_cast<int16_t>(v);
    }
    *ph1 = h1;
    *ph2 = h2;
}

// ------------------------------------------------------------ --audio-selftest (ctest sly2-audio; silent: run it with
// SDL_AUDIODRIVER=disk, the output into SDL_DISKAUDIOFILE, the microphone from a file this test writes)
#ifdef SLY2_AUDIO_DEVICES
namespace
{
std::vector<std::string> s_aszFakeOut, s_aszFakeIn;
std::vector<std::string> FakeList(bool fCapture) { return fCapture ? s_aszFakeIn : s_aszFakeOut; }
}  // namespace
#endif

int AudioSelfTest(const char* szDir)
{
    int cfail = 0, ccheck = 0;
    auto check = [&](bool f, const char* what, const std::string& detail = {}) {
        ++ccheck;
        if (f) return;
        ++cfail;
        std::printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
    };
#ifdef SLY2_AUDIO_DEVICES
    namespace fs = std::filesystem;
    const fs::path dir = szDir && *szDir ? fs::path(szDir) : fs::temp_directory_path();
    fs::create_directories(dir);
    // the microphone's level (spiop.irx 0x11B4..0x1210): silence, a square wave, a sine
    {
        std::vector<int16_t> a(1024, 0);
        AddMicSamples(a.data(), int(a.size()));
        check(s_gMic.load() == 0.0f, "mic level: silence 0");
        for (size_t i = 0; i < a.size(); ++i) a[i] = int16_t(i & 1 ? 26000 : -26000);
        AddMicSamples(a.data(), 512);
        check(s_gMic.load() == 0.0f, "mic level: a window is 1024 samples");
        AddMicSamples(a.data() + 512, 512);
        check(std::fabs(s_gMic.load() - 26000.0f / 32768.0f) < 1e-4f, "mic level: square wave 26000", std::to_string(s_gMic.load()));
        for (size_t i = 0; i < a.size(); ++i) a[i] = int16_t(std::lround(20000.0 * std::sin(i * 0.05)));
        AddMicSamples(a.data(), 1024);
        check(std::fabs(s_gMic.load() - 20000.0f / 32768.0f / std::sqrt(2.0f)) < 0.01f, "mic level: sine RMS", std::to_string(s_gMic.load()));
        s_gMic.store(0.0f);
    }
    // the noise's radius / height by level (0x1FCD38: FitClq through (0, 0), (0.5, 500), (1, 3000 / 1000))
    // (sound.h SOUNDMGR clqMicRadius / clqMicHeight hold these; checked in sound.cpp's terms by the game test)
    // a loud microphone file for SDL's disk capture (16-bit mono, the format OpenMic asks for)
    const fs::path fileIn = dir / "audio-selftest-mic.raw";
    {
        std::vector<int16_t> a(22050 * 6);
        for (size_t i = 0; i < a.size(); ++i) a[i] = int16_t(i & 1 ? 26000 : -26000);
        std::FILE* pf = std::fopen(cane::utf8(fileIn).c_str(), "wb");
        if (pf)
        {
            std::fwrite(a.data(), 2, a.size(), pf);
            std::fclose(pf);
        }
    }
    const char* szDriver = std::getenv("SDL_AUDIODRIVER");
    const bool fDisk = szDriver && !std::strcmp(szDriver, "disk");
    if (!fDisk) std::printf("audio self-test: SDL_AUDIODRIVER is not disk: the device / microphone part is skipped\n");
    else
    {
#ifdef _WIN32
        _putenv_s("SDL_DISKAUDIOFILEIN", cane::utf8(fileIn).c_str());
#else
        setenv("SDL_DISKAUDIOFILEIN", cane::utf8(fileIn).c_str(), 1);
#endif
        check(FEnsureAudio() && s_pbus, "engine, mastering voice and bus");
        const std::vector<std::string> real = AszDevices(false);
        std::printf("audio self-test: SDL driver %s, outputs:", SDL_GetCurrentAudioDriver());
        for (const auto& n : real) std::printf(" \"%s\"", n.c_str());
        std::printf("\n");
        check(!real.empty(), "SDL lists an output");
        const std::string name = real.empty() ? std::string() : real[0];
        // a tone playing through the bus the whole time: its frames keep being consumed across every device change
        CPcmStream tone;
        check(tone.FOpen(1, 48000), "a stream opens");
        std::vector<int16_t> buf(4800);
        for (size_t i = 0; i < buf.size(); ++i) buf[i] = int16_t(8000.0 * std::sin(i * 0.1));
        tone.Start();
        auto pump = [&](int ms) {
            const auto tEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < tEnd)
            {
                while (tone.FCanPush()) tone.FPush(buf.data(), buf.size());
                SDL_Delay(5);
            }
        };
        pump(150);
        uint64_t played = tone.CFramePlayed();
        check(played > 0, "the stream plays on the default device");
        // a chosen device that is not listed (unplugged): the default; listed (plugged in): taken; gone again: the default
        s_pfnListHook = FakeList;
        s_aszFakeOut = {"Some Other Device"};
        SetAudioDevices(name, "", "");
        check(SzAudioOutputOpen().empty(), "a missing chosen device: the default", SzAudioOutputOpen());
        s_aszFakeOut = {"Some Other Device", name};
        PollAudioDevices(true);
        check(SzAudioOutputOpen() == name, "the chosen device plugged in: taken", SzAudioOutputOpen());
        pump(150);
        check(tone.CFramePlayed() > played, "the stream keeps playing on the new device");
        played = tone.CFramePlayed();
        s_aszFakeOut = {"Some Other Device"};
        PollAudioDevices(true);
        check(SzAudioOutputOpen().empty(), "the chosen device unplugged: back on the default", SzAudioOutputOpen());
        pump(150);
        check(tone.CFramePlayed() > played, "the stream keeps playing after the fallback");
        played = tone.CFramePlayed();
        s_aszFakeOut = {name};
        PollAudioDevices(true);
        check(SzAudioOutputOpen() == name, "the chosen device back: taken again", SzAudioOutputOpen());
        pump(150);
        check(tone.CFramePlayed() > played, "the stream keeps playing after the recovery");
        SetAudioDevices("", "", "");
        check(SzAudioOutputOpen().empty(), "System default chosen again");
        // the microphone: not open until the headset is used; then recorded (here: the loud file), its level as the IRX's
        s_pfnListHook = nullptr;
        check(GMicLevel() == 0.0f && !s_devMic, "no microphone while the headset is off");
        SetMicCapture(true);
        check(s_devMic != 0, "the microphone opens with the headset on");
        SDL_Delay(400);
        check(std::fabs(GMicLevel() - 26000.0f / 32768.0f) < 1e-3f, "the microphone's level from the disk capture", std::to_string(GMicLevel()));
        // a chosen microphone that is missing: the default; the headset off: closed, level 0
        SetAudioDevices("", "Missing Microphone", "");
        PollAudioDevices(true);
        check(s_devMic != 0 && s_devIn.open.empty(), "a missing chosen microphone: the default");
        SetMicCapture(false);
        check(s_devMic == 0 && GMicLevel() == 0.0f, "the headset off: the microphone closed");
        // the headset's speaker: only with a chosen device that is not the game's output
        SetMicCapture(true);
        SetAudioDevices("", "", name);
        PollAudioDevices(true);
        check(FHeadsetOutputReady() == !name.empty(), "the headset output opens on its device");
        CPcmStream line;
        check(line.FOpen(1, 22050, 4.0f, true), "a dialogue line opens on the headset");
        line.Close();
        SetAudioDevices("", "", "Missing Headset");
        PollAudioDevices(true);
        check(!FHeadsetOutputReady(), "a missing headset device: the lines stay on the game's output");
        SetMicCapture(false);
        tone.Close();
        ShutdownAudio();
        std::error_code ec;
        fs::remove(fileIn, ec);
    }
#else
    (void)szDir;
    std::printf("audio self-test: no SDL devices on this platform\n");
#endif
    std::printf("audio self-test: %d checks, %d failed\n", ccheck, cfail);
    return cfail ? 1 : 0;
}
