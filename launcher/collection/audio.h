#pragma once
// The Collection Style Launcher's sound: the menu's music and its short interface sounds, mixed in one SDL2 audio
// callback. SDL2 is loaded at run time (SDL2.dll beside the games on Windows, the system's
// libSDL2-2.0.so.0 on Linux): without it the collection runs silently, and the main launcher never needs it.
// Decoders for the formats the menu reads: Xbox IMA ADPCM (The Sly Collection's FMOD banks, FSB4: at the import) and
// PCM WAV (the converted menu's music and sounds).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace cane::collection {
// Interleaved stereo 16-bit PCM with an optional loop (in frames; loopEnd 0 = the end).
struct Pcm {
    int rate = 44100;
    std::vector<int16_t> samples;
    size_t loopStart = 0, loopEnd = 0;
    bool loops = true;
    size_t frames() const { return samples.size() / 2; }
};
using PcmPtr = std::shared_ptr<const Pcm>;

// ---------------------------------------------------------------- decoders
namespace detail {
inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
// One IMA ADPCM nibble.
struct Ima {
    int predictor = 0, index = 0;
    int16_t step(int nibble) {
        static constexpr int16_t steps[89] = {
            7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,    31,
            34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,
            157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
            724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
            3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
            15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
        static constexpr int8_t indexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
        const int s = steps[index];
        int diff = s >> 3;
        if (nibble & 4) diff += s;
        if (nibble & 2) diff += s >> 1;
        if (nibble & 1) diff += s >> 2;
        predictor = std::clamp(predictor + ((nibble & 8) ? -diff : diff), -32768, 32767);
        index = std::clamp(index + indexTable[nibble & 15], 0, 88);
        return int16_t(predictor);
    }
};
}  // namespace detail

// Xbox IMA ADPCM: per channel 36-byte blocks (a 4-byte header, then 64 samples), stereo blocks interleaving 4-byte
// groups of each channel after both headers (as Microsoft IMA ADPCM). Returns stereo PCM.
inline Pcm decodeXboxIma(const uint8_t* data, size_t size, int channels, int rate, size_t frames) {
    Pcm out;
    out.rate = rate;
    if (channels < 1 || channels > 2) return out;
    const size_t block = 36u * size_t(channels);
    const size_t blocks = size / block;
    out.samples.reserve(std::min(frames, blocks * 64) * 2);
    std::vector<int16_t> ch[2];
    for (size_t b = 0; b < blocks && out.frames() < frames; ++b) {
        const uint8_t* p = data + b * block;
        detail::Ima ima[2];
        for (int c = 0; c < channels; ++c) {
            ima[c].predictor = int16_t(detail::le16(p + 4 * c));
            ima[c].index = std::min<int>(p + 4 * c + 2 < data + size ? p[4 * c + 2] : 0, 88);
            ch[c].assign(64, 0);
        }
        const uint8_t* q = p + 4 * channels;
        for (int group = 0; group < 8; ++group)
            for (int c = 0; c < channels; ++c)
                for (int k = 0; k < 4; ++k, ++q) {
                    ch[c][size_t(group * 8 + k * 2)] = ima[c].step(*q & 15);
                    ch[c][size_t(group * 8 + k * 2 + 1)] = ima[c].step(*q >> 4);
                }
        for (size_t i = 0; i < 64 && out.frames() < frames; ++i) {
            out.samples.push_back(ch[0][i]);
            out.samples.push_back(ch[channels - 1][i]);
        }
    }
    out.loopEnd = out.frames();
    return out;
}

// FSB4 (FMOD Ex sample bank): the samples' names and where their data is.
struct FsbSample {
    std::string name;
    size_t offset = 0, size = 0, frames = 0, loopStart = 0, loopEnd = 0;
    uint32_t mode = 0;
    int rate = 44100, channels = 1;
};
inline std::vector<FsbSample> readFsb(const std::vector<uint8_t>& d) {
    std::vector<FsbSample> list;
    if (d.size() < 48 || std::memcmp(d.data(), "FSB4", 4) != 0) return list;
    const uint32_t count = detail::le32(&d[4]), headers = detail::le32(&d[8]);
    size_t at = 48, data = 48 + size_t(headers);
    for (uint32_t i = 0; i < count && at + 64 <= d.size(); ++i) {
        const uint16_t size = detail::le16(&d[at]);
        if (size < 64) break;
        FsbSample s;
        s.name.assign(reinterpret_cast<const char*>(&d[at + 2]), strnlen(reinterpret_cast<const char*>(&d[at + 2]), 30));
        s.frames = detail::le32(&d[at + 32]);
        s.size = detail::le32(&d[at + 36]);
        s.loopStart = detail::le32(&d[at + 40]);
        s.loopEnd = detail::le32(&d[at + 44]);
        s.mode = detail::le32(&d[at + 48]);
        s.rate = int(detail::le32(&d[at + 52]));
        s.channels = detail::le16(&d[at + 62]);
        s.offset = data;
        data = (data + s.size + 31) & ~size_t(31);  // (each sample's data starts 32-byte aligned)
        at += size;
        if (s.offset + s.size <= d.size()) list.push_back(std::move(s));
    }
    return list;
}
// A sample of the bank as PCM (IMA ADPCM only: the collection's banks; anything else comes back empty).
inline Pcm decodeFsbSample(const std::vector<uint8_t>& bank, const FsbSample& s) {
    constexpr uint32_t kImaAdpcm = 0x00400000;
    if (!(s.mode & kImaAdpcm)) return {};
    Pcm p = decodeXboxIma(bank.data() + s.offset, s.size, s.channels, s.rate, s.frames);
    if (s.loopEnd > s.loopStart && s.loopEnd < p.frames()) p.loopStart = s.loopStart, p.loopEnd = s.loopEnd + 1;
    return p;
}

// A PCM WAV file (16-bit, any channel count; WAVE_FORMAT_EXTENSIBLE too) as stereo: two channels as they are, Sly 2's
// stem layouts by their "normal" stem (5 channels: sneak, normal L/R, alert L/R; 3: sneak, normal L/R). Reads the
// smpl chunk's loop. Empty on anything else.
inline Pcm decodeWav(const std::vector<uint8_t>& d) {
    Pcm out;
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(&d[8], "WAVE", 4) != 0) return out;
    int channels = 0, bits = 0;
    uint32_t loopStart = 0, loopEnd = 0;
    const uint8_t* pcm = nullptr;
    size_t pcmSize = 0;
    for (size_t at = 12; at + 8 <= d.size();) {
        const uint32_t size = detail::le32(&d[at + 4]);
        const uint8_t* body = &d[at + 8];
        const size_t avail = std::min<size_t>(size, d.size() - at - 8);
        if (!std::memcmp(&d[at], "fmt ", 4) && avail >= 16) {
            const uint16_t tag = detail::le16(body);
            channels = detail::le16(body + 2);
            out.rate = int(detail::le32(body + 4));
            bits = detail::le16(body + 14);
            if (tag != 1 && tag != 0xFFFE) return {};
        } else if (!std::memcmp(&d[at], "data", 4)) {
            pcm = body;
            pcmSize = avail;
        } else if (!std::memcmp(&d[at], "smpl", 4) && avail >= 36 + 24 && detail::le32(body + 28) > 0) {
            loopStart = detail::le32(body + 36 + 8);
            loopEnd = detail::le32(body + 36 + 12);
        }
        at += 8 + size + (size & 1);
    }
    if (!pcm || bits != 16 || channels < 1) return {};
    int left = 0, right = channels > 1 ? 1 : 0;
    if (channels == 5 || channels == 3) left = 1, right = 2;
    const size_t frames = pcmSize / (2u * size_t(channels));
    out.samples.resize(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        const uint8_t* s = pcm + f * 2u * size_t(channels);
        out.samples[f * 2] = int16_t(detail::le16(s + 2 * left));
        out.samples[f * 2 + 1] = int16_t(detail::le16(s + 2 * right));
    }
    out.loopStart = loopStart < frames ? loopStart : 0;
    out.loopEnd = loopEnd > out.loopStart && loopEnd < frames ? loopEnd + 1 : frames;
    return out;
}

// ---------------------------------------------------------------- output (SDL2, loaded at run time)
class Audio {
public:
    static constexpr int kRate = 44100;
    Audio() = default;
    Audio(const Audio&) = delete;
    ~Audio() { close(); }
    // Opens the output: the device of this name when it is there (the one chosen for the games), else the system's.
    // False (and silent) without SDL2 or an audio device.
    bool open(const std::string& deviceName = {}) {
        if (m_device) return true;
        if (!load()) return false;
        if (m_init(kInitAudio) != 0) return false;
        Spec want{};
        want.freq = kRate;
        want.format = 0x8010;  // AUDIO_S16SYS (little endian)
        want.channels = 2;
        want.samples = 1024;
        want.callback = &Audio::callback;
        want.userdata = this;
        Spec have{};
        if (!deviceName.empty()) m_device = m_openDevice(deviceName.c_str(), 0, &want, &have, 0);
        if (!m_device) m_device = m_openDevice(nullptr, 0, &want, &have, 0);
        if (!m_device) {
            m_quit(kInitAudio);
            return false;
        }
        m_pause(m_device, 0);
        return true;
    }
    void close() {
        if (m_device) {
            m_closeDevice(m_device);
            m_device = 0;
            m_quit(kInitAudio);
        }
    }
    bool available() const { return m_device != 0; }
    // Crossfades to a track (nullptr: fades out). The same track keeps playing.
    void playMusic(PcmPtr track, float fadeSeconds = 1.2f) {
        std::lock_guard lock(m_mutex);
        if (m_music[0].pcm == track) return;
        m_music[1] = m_music[0];
        m_music[1].fade = -1.0f / std::max(0.05f, fadeSeconds);
        m_music[0] = Voice{track, 0.0, 0.0f, 1.0f / std::max(0.05f, fadeSeconds)};
    }
    // Moves the playing track to a place (seconds): the audio check listens across its loop point.
    void seekMusic(double seconds) {
        std::lock_guard lock(m_mutex);
        if (m_music[0].pcm) m_music[0].pos = std::clamp(seconds * double(m_music[0].pcm->rate), 0.0, double(m_music[0].pcm->frames()) - 2.0);
    }
    PcmPtr music() const {
        std::lock_guard lock(m_mutex);
        return m_music[0].pcm;
    }
    void playSound(PcmPtr sound, float volume = 1.0f) {
        if (!sound) return;
        std::lock_guard lock(m_mutex);
        for (auto& v : m_sounds)
            if (!v.pcm) {
                v = Voice{sound, 0.0, volume, 0.0f};
                return;
            }
        m_sounds[0] = Voice{sound, 0.0, volume, 0.0f};
    }
    void setVolumes(float music, float sounds) {
        m_musicVolume = std::clamp(music, 0.0f, 1.0f);
        m_soundVolume = std::clamp(sounds, 0.0f, 1.0f);
    }
    // Keeps playing but silent (while a game runs).
    void setMuted(bool muted) { m_muted = muted; }

private:
    static constexpr uint32_t kInitAudio = 0x10;
    struct Spec {
        int freq;
        uint16_t format;
        uint8_t channels, silence;
        uint16_t samples, padding;
        uint32_t size;
        void (*callback)(void*, uint8_t*, int);
        void* userdata;
    };
    struct Voice {
        PcmPtr pcm;
        double pos = 0;
        float gain = 0;
        float fade = 0;  // gain change per second (music)
    };
    bool load() {
#ifdef _WIN32
        HMODULE lib = LoadLibraryW((executableDirectory() / L"SDL2.dll").c_str());
        if (!lib) lib = LoadLibraryW(L"SDL2.dll");
        if (!lib) return false;
        auto sym = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(lib, n)); };
#else
        void* lib = nullptr;
        for (const fs::path& p : {executableDirectory() / "libSDL2-2.0.so.0", executableDirectory().parent_path() / "lib" / "libSDL2-2.0.so.0",
                                  fs::path("libSDL2-2.0.so.0"), fs::path("libSDL2-2.0.0.dylib"), fs::path("libSDL2.so")})
            if ((lib = dlopen(utf8(p).c_str(), RTLD_NOW | RTLD_LOCAL))) break;
        if (!lib) return false;
        auto sym = [&](const char* n) { return dlsym(lib, n); };
#endif
        m_init = reinterpret_cast<int (*)(uint32_t)>(sym("SDL_InitSubSystem"));
        m_quit = reinterpret_cast<void (*)(uint32_t)>(sym("SDL_QuitSubSystem"));
        m_openDevice = reinterpret_cast<uint32_t (*)(const char*, int, const Spec*, Spec*, int)>(sym("SDL_OpenAudioDevice"));
        m_pause = reinterpret_cast<void (*)(uint32_t, int)>(sym("SDL_PauseAudioDevice"));
        m_closeDevice = reinterpret_cast<void (*)(uint32_t)>(sym("SDL_CloseAudioDevice"));
        return m_init && m_quit && m_openDevice && m_pause && m_closeDevice;
    }
    // One voice into the float mix; false when it ended.
    static bool mix(Voice& v, float* out, int frames, float volume, bool music) {
        if (!v.pcm || v.pcm->samples.empty()) return false;
        const Pcm& p = *v.pcm;
        const double step = double(p.rate) / kRate;
        const size_t end = p.loopEnd ? std::min(p.loopEnd, p.frames()) : p.frames();
        const float dt = 1.0f / kRate;
        for (int i = 0; i < frames; ++i) {
            if (music) {
                v.gain = std::clamp(v.gain + v.fade * dt, 0.0f, 1.0f);
                if (v.fade < 0 && v.gain <= 0) return false;
            }
            size_t f = size_t(v.pos);
            if (f + 1 >= end) {
                if (!p.loops) return false;
                v.pos = double(p.loopStart) + (v.pos - double(end - 1));
                f = size_t(v.pos);
                if (f + 1 >= end) return false;
            }
            const float t = float(v.pos - double(f));
            const float g = v.gain * volume / 32768.0f;
            out[i * 2] += g * (float(p.samples[f * 2]) * (1 - t) + float(p.samples[f * 2 + 2]) * t);
            out[i * 2 + 1] += g * (float(p.samples[f * 2 + 1]) * (1 - t) + float(p.samples[f * 2 + 3]) * t);
            v.pos += step;
        }
        return true;
    }
    static void callback(void* user, uint8_t* stream, int bytes) {
        auto& a = *static_cast<Audio*>(user);
        const int frames = bytes / 4;
        a.m_mix.assign(size_t(frames) * 2, 0.0f);
        {
            std::lock_guard lock(a.m_mutex);
            const float mv = a.m_muted ? 0.0f : a.m_musicVolume.load() * 0.8f, sv = a.m_muted ? 0.0f : a.m_soundVolume.load();
            for (auto& v : a.m_music)
                if (!mix(v, a.m_mix.data(), frames, mv, true)) v = {};
            for (auto& v : a.m_sounds)
                if (!mix(v, a.m_mix.data(), frames, sv, false)) v = {};
        }
        auto* out = reinterpret_cast<int16_t*>(stream);
        for (size_t i = 0; i < a.m_mix.size(); ++i) out[i] = int16_t(std::clamp(a.m_mix[i], -1.0f, 1.0f) * 32767.0f);
    }
    int (*m_init)(uint32_t) = nullptr;
    void (*m_quit)(uint32_t) = nullptr;
    uint32_t (*m_openDevice)(const char*, int, const Spec*, Spec*, int) = nullptr;
    void (*m_pause)(uint32_t, int) = nullptr;
    void (*m_closeDevice)(uint32_t) = nullptr;
    uint32_t m_device = 0;
    mutable std::mutex m_mutex;
    Voice m_music[2];
    Voice m_sounds[8];
    std::vector<float> m_mix;
    std::atomic<float> m_musicVolume{0.7f}, m_soundVolume{0.8f};
    std::atomic<bool> m_muted{false};
};
}  // namespace cane::collection
