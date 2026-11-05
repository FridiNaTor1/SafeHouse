// See musmod.h.
#include "musmod.h"

#include "../platform/platform.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace fs = std::filesystem;

bool PlayMusicFileForCapture(const std::filesystem::path& path, double seconds);   // (sound.cpp)
#ifdef _WIN32
#include <objbase.h>
void SpuSetOutputCapture(std::vector<float>* sink);   // (spu_reverb_xapo.cpp)
#endif

namespace
{
constexpr int kRate = 48000;             // a tone at its centre note plays at 48 kHz (sound.cpp StartMusicTone)
constexpr int kBlockSamples = 28;        // samples per 16-byte SPU ADPCM block
constexpr int kCoefficients[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

struct WAVE
{
    int rate = 0;
    std::vector<std::vector<float>> channels;   // -1..1
};

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

WAVE ReadWav(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)), {});
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 || std::memcmp(file.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("not a WAV file: " + cane::utf8(path));
    int format = 0, channels = 0, bits = 0, rate = 0;
    const uint8_t* data = nullptr;
    std::size_t dataSize = 0;
    for (std::size_t offset = 12; offset + 8 <= file.size();)
    {
        const uint32_t size = Le32(file.data() + offset + 4);
        const uint8_t* body = file.data() + offset + 8;
        const std::size_t available = std::min<std::size_t>(size, file.size() - offset - 8);
        if (std::memcmp(file.data() + offset, "fmt ", 4) == 0 && available >= 16)
        {
            format = Le16(body);
            channels = Le16(body + 2);
            rate = int(Le32(body + 4));
            bits = Le16(body + 14);
            if (format == 0xFFFE && available >= 26) format = Le16(body + 24);   // (WAVE_FORMAT_EXTENSIBLE)
        }
        else if (std::memcmp(file.data() + offset, "data", 4) == 0)
        {
            data = body;
            dataSize = available;
        }
        offset += 8 + size + (size & 1);
    }
    const bool pcm = format == 1 && (bits == 16 || bits == 24);
    const bool floating = format == 3 && bits == 32;
    if (!data || channels < 1 || channels > 2 || rate < 4000 || rate > 192000 || (!pcm && !floating))
        throw std::runtime_error("unsupported WAV (use 16 / 24-bit PCM or 32-bit float, mono or stereo): " + cane::utf8(path));
    WAVE wave;
    wave.rate = rate;
    wave.channels.assign(channels, {});
    const std::size_t frameBytes = std::size_t(channels) * bits / 8;
    const std::size_t frames = dataSize / frameBytes;
    for (auto& channel : wave.channels) channel.resize(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
        for (int c = 0; c < channels; ++c)
        {
            const uint8_t* p = data + frame * frameBytes + std::size_t(c) * bits / 8;
            float value = 0.0f;
            if (floating) std::memcpy(&value, p, 4);
            else if (bits == 16) value = int16_t(Le16(p)) / 32768.0f;
            else value = float(int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) >> 8) / 8388608.0f;
            wave.channels[c][frame] = value;
        }
    return wave;
}

// Catmull-Rom between x[i] and x[i + 1]; `at` maps an index to a sample (clamping or wrapping).
template <typename AT>
float Cubic(AT at, long i, float t)
{
    const float p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
    return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
}

// The channel at 48 kHz, laid out for the SPU: a loop (when looping) starting on a block and lasting whole blocks.
// `introBlocks` / `loopBlocks` give the layout.
std::vector<float> Layout(const std::vector<float>& x, int rate, bool loop, double loopSeconds,
                          std::size_t& introSamples, std::size_t& loopSamples)
{
    const long n = long(x.size());
    auto clamped = [&](long i) { return x[std::clamp<long>(i, 0, n - 1)]; };
    const double step = double(rate) / kRate;   // input samples per output sample
    std::vector<float> out;
    if (!loop)
    {
        const std::size_t count = std::size_t(std::llround(n / step));
        for (std::size_t i = 0; i < count; ++i)
        {
            const double position = i * step;
            out.push_back(Cubic(clamped, long(position), float(position - std::floor(position))));
        }
        out.resize((out.size() + kBlockSamples - 1) / kBlockSamples * kBlockSamples, 0.0f);
        introSamples = out.size();
        loopSamples = 0;
        return out;
    }
    const long loopStart = std::clamp<long>(long(std::llround(loopSeconds * rate)), 0, n - 1);
    const long body = n - loopStart;
    // the intro, at 48 kHz, delayed by under one block so the loop starts on a block
    const std::size_t intro = std::size_t(std::llround(loopStart / step));
    const std::size_t pad = (kBlockSamples - intro % kBlockSamples) % kBlockSamples;
    out.assign(pad, 0.0f);
    for (std::size_t i = 0; i < intro; ++i)
    {
        const double position = i * step;
        out.push_back(Cubic(clamped, long(position), float(position - std::floor(position))));
    }
    // the loop, resampled (wrapping) to a whole number of blocks: under 14 samples of difference
    const std::size_t blocks = std::max<std::size_t>(1, std::size_t(std::llround(body / step / kBlockSamples)));
    const std::size_t length = blocks * kBlockSamples;
    auto wrapped = [&](long i) { return x[loopStart + ((i % body) + body) % body]; };
    const double loopStep = double(body) / length;
    for (std::size_t i = 0; i < length; ++i)
    {
        const double position = i * loopStep;
        out.push_back(Cubic(wrapped, long(position), float(position - std::floor(position))));
    }
    introSamples = pad + intro;
    loopSamples = length;
    return out;
}

int Decode(int nibble, int shift, int filter, int h1, int h2)
{
    int value = (nibble * 4096) >> shift;
    value += (kCoefficients[filter][0] * h1 + kCoefficients[filter][1] * h2 + 32) >> 6;
    return std::clamp(value, -32768, 32767);
}

// One block, choosing the filter and shift with the least error against the game's own decoder arithmetic.
// With `other` (the loop's first block): the block is also entered with that history (the loop's end), and the
// error of both entries counts, so it decodes nearly the same either way.
void EncodeBlock(const float* x, int& h1, int& h2, uint8_t flags, const int* other, uint8_t* out)
{
    double bestError = std::numeric_limits<double>::max();
    int bestFilter = 0, bestShift = 0;
    uint8_t bestNibbles[kBlockSamples]{};
    int bestH1 = h1, bestH2 = h2;
    int target[kBlockSamples];
    for (int i = 0; i < kBlockSamples; ++i) target[i] = std::clamp(int(std::lround(x[i] * 32767.0f)), -32768, 32767);
    for (int filter = 0; filter < 5; ++filter)
        for (int shift = 0; shift <= 12; ++shift)
        {
            int a = h1, b = h2;
            int oa = other ? other[0] : 0, ob = other ? other[1] : 0;
            double error = 0.0;
            uint8_t nibbles[kBlockSamples];
            for (int i = 0; i < kBlockSamples; ++i)
            {
                const int predicted = (kCoefficients[filter][0] * a + kCoefficients[filter][1] * b + 32) >> 6;
                const int guess = std::clamp(int(std::lround(double(target[i] - predicted) * (1 << shift) / 4096.0)), -8, 7);
                int best = guess;
                double bestCost = std::numeric_limits<double>::max();
                int bestValue = 0, bestOther = 0;
                for (int candidate : {guess - 1, guess, guess + 1})
                {
                    if (candidate < -8 || candidate > 7) continue;
                    const int value = Decode(candidate, shift, filter, a, b);
                    const int otherValue = other ? Decode(candidate, shift, filter, oa, ob) : value;
                    const double cost = double(value - target[i]) * (value - target[i]) +
                        (other ? double(otherValue - target[i]) * (otherValue - target[i]) : 0.0);
                    if (cost < bestCost) { bestCost = cost; best = candidate; bestValue = value; bestOther = otherValue; }
                }
                error += bestCost;
                nibbles[i] = uint8_t(best & 15);
                b = a;
                a = bestValue;
                ob = oa;
                oa = bestOther;
                if (error >= bestError) break;
            }
            if (error < bestError)
            {
                bestError = error;
                bestFilter = filter;
                bestShift = shift;
                std::memcpy(bestNibbles, nibbles, sizeof(nibbles));
                bestH1 = a;
                bestH2 = b;
            }
        }
    out[0] = uint8_t((bestFilter << 4) | bestShift);
    out[1] = flags;
    for (int i = 0; i < kBlockSamples / 2; ++i) out[2 + i] = uint8_t(bestNibbles[2 * i] | (bestNibbles[2 * i + 1] << 4));
    h1 = bestH1;
    h2 = bestH2;
}

// The SPU goes on at the loop's first block with the decoder history of the loop's end; the game (sound.cpp
// DecodeMusicAdpcm) decodes the loop again with that history until it repeats. The loop's first block is encoded
// for both histories it is entered with (the intro's end, the loop's end), a few rounds, so the passes agree.
std::vector<uint8_t> EncodeAdpcm(const std::vector<float>& samples, std::size_t introSamples, std::size_t loopSamples)
{
    const std::size_t blocks = samples.size() / kBlockSamples;
    const std::size_t loopBlock = introSamples / kBlockSamples;
    std::vector<uint8_t> out(blocks * 16);
    auto flagsOf = [&](std::size_t block) {
        uint8_t flags = 0;
        if (loopSamples != 0 && block == loopBlock) flags |= 0x04;
        if (block + 1 == blocks) flags |= loopSamples != 0 ? 0x03 : 0x01;
        return flags;
    };
    int h1 = 0, h2 = 0;
    for (std::size_t block = 0; block < loopBlock; ++block)
        EncodeBlock(samples.data() + block * kBlockSamples, h1, h2, flagsOf(block), nullptr, out.data() + block * 16);
    const int introH1 = h1, introH2 = h2;
    int loopEnd[2] = {introH1, introH2};
    for (int round = 0; round < (loopSamples != 0 ? 4 : 1); ++round)
    {
        h1 = introH1;
        h2 = introH2;
        for (std::size_t block = loopBlock; block < blocks; ++block)
            EncodeBlock(samples.data() + block * kBlockSamples, h1, h2, flagsOf(block),
                loopSamples != 0 && block == loopBlock && round > 0 ? loopEnd : nullptr, out.data() + block * 16);
        loopEnd[0] = h1;
        loopEnd[1] = h2;
    }
    return out;
}

void Put32(std::vector<uint8_t>& v, std::size_t at, uint32_t x) { for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(x >> (8 * i)); }
void Put16(std::vector<uint8_t>& v, std::size_t at, uint16_t x) { v[at] = uint8_t(x); v[at + 1] = uint8_t(x >> 8); }
void Align(std::vector<uint8_t>& v, std::size_t to) { v.resize((v.size() + to - 1) / to * to, 0); }

// A 989snd music bank as sound.cpp LoadMusic989Mus reads it: one program with one tone per channel (left / right),
// and one sequence that keys the note once (the tones loop by their ADPCM flags) and then waits about 97 days.
std::vector<uint8_t> BuildMus(const std::vector<std::vector<uint8_t>>& tones, int volume)
{
    constexpr std::size_t kSblk = 0x20;
    const std::size_t toneCount = tones.size();
    std::vector<uint8_t> file(kSblk, 0);
    // SBv2: header 0x34, one cue (0x1c), one program (8), the tones (0x18 each)
    const std::size_t sblkHeader = 0x34, cueTable = sblkHeader, programTable = cueTable + 0x1c,
                      toneTable = programTable + 8;
    std::vector<uint8_t> sblk(toneTable + toneCount * 0x18, 0);
    std::memcpy(sblk.data(), "SBv2", 4);
    Put32(sblk, 4, 2);
    Put32(sblk, 8, 4);
    std::memcpy(sblk.data() + 12, "MODS", 4);
    Put16(sblk, 0x14, 1);                        // cues
    Put16(sblk, 0x16, 1);                        // programs
    Put16(sblk, 0x18, uint16_t(toneCount));
    Put16(sblk, 0x1a, uint16_t(toneCount));
    Put32(sblk, 0x1c, uint32_t(cueTable));
    Put32(sblk, 0x20, uint32_t(programTable));
    Put32(sblk, 0x24, uint32_t(toneTable));
    // the cue: volume 127, repeats 0 (= forever), volume group 1, pan 0
    Put32(sblk, cueTable, 5);
    for (int i = 0; i < 3; ++i) std::memcpy(sblk.data() + cueTable + 4 + 4 * i, "MODS", 4);
    sblk[cueTable + 0x10] = 127;
    sblk[cueTable + 0x13] = 1;
    sblk[cueTable + 0x17] = 2;
    // the program
    sblk[programTable] = uint8_t(toneCount);
    sblk[programTable + 1] = uint8_t(std::clamp(volume, 0, 127));
    Put32(sblk, programTable + 4, uint32_t(toneTable));
    uint32_t streamOffset = 0;
    for (std::size_t t = 0; t < toneCount; ++t)
    {
        const std::size_t e = toneTable + t * 0x18;
        sblk[e + 0] = 127;                                    // priority
        sblk[e + 1] = 127;                                    // volume
        sblk[e + 2] = uint8_t(int8_t(-60));                   // centre note 60, PS2 tuning: note 60 plays at 48 kHz
        sblk[e + 3] = 0;                                      // fine tune
        Put16(sblk, e + 4, toneCount == 1 ? 0 : (t == 0 ? 270 : 90));   // pan: left, right
        sblk[e + 6] = 0;                                      // key range
        sblk[e + 7] = 127;
        Put16(sblk, e + 0x0a, 0x00ff);                        // ADSR: instant attack, full sustain
        Put16(sblk, e + 0x0c, 0x1fcb);
        Put32(sblk, e + 0x10, streamOffset);
        Put32(sblk, e + 0x14, uint32_t(t));
        streamOffset += uint32_t(tones[t].size());
    }
    Align(sblk, 16);
    // the sequence: MMID with one MID segment
    std::vector<uint8_t> sequence(0x10, 0);
    Put32(sequence, 0, 2);
    Put32(sequence, 4, 1);
    Put32(sequence, 8, 0x10);
    const std::size_t mmid = sequence.size();
    sequence.resize(mmid + 0x20, 0);
    std::memcpy(sequence.data() + mmid, "MMID", 4);
    sequence[mmid + 4] = 1;
    sequence[mmid + 7] = 1;                                   // segments
    std::memcpy(sequence.data() + mmid + 8, "MODS", 4);
    Put32(sequence, mmid + 16, 0x20);                         // the segment, from MMID
    const std::size_t mid = sequence.size();
    sequence.resize(mid + 0x30, 0);
    std::memcpy(sequence.data() + mid, "MID ", 4);
    sequence[mid + 4] = 1;
    sequence[mid + 6] = 8;
    std::memcpy(sequence.data() + mid + 16, "MODS", 4);
    Put32(sequence, mid + 24, 0x30);                          // events, from MID
    Put32(sequence, mid + 32, 1000000);                       // tempo: 1 s per quarter note
    Put32(sequence, mid + 36, 1);                             // 1 tick per quarter note
    const uint8_t events[] = {
        0x00, 0xC0, 0x00,                                     // program 0
        0x00, 0x90, 60, 127,                                  // note 60 on
        0x83, 0xFF, 0xFF, 0x7F,                               // wait 0x7FFFFF quarter notes (~97 days; the
        0xFF, 0x2F, 0x00};                                    //  sequencer's tick counter holds no more)
    sequence.insert(sequence.end(), std::begin(events), std::end(events));
    Align(sequence, 16);
    Put32(sequence, 0x0c, uint32_t(sequence.size() - mmid));

    file.insert(file.end(), sblk.begin(), sblk.end());
    const std::size_t audio = file.size();
    for (const auto& tone : tones) file.insert(file.end(), tone.begin(), tone.end());
    Align(file, 16);
    const std::size_t audioSize = streamOffset;
    const std::size_t sequenceOffset = file.size();
    file.insert(file.end(), sequence.begin(), sequence.end());
    Put32(file, 0x00, 1);
    Put32(file, 0x04, 3);
    Put32(file, 0x08, uint32_t(kSblk));
    Put32(file, 0x0c, uint32_t(sblk.size()));
    Put32(file, 0x10, uint32_t(audio));
    Put32(file, 0x14, uint32_t(audioSize));
    Put32(file, 0x18, uint32_t(sequenceOffset));
    Put32(file, 0x1c, uint32_t(sequence.size()));
    // (the bank's audio size, as the original banks record it)
    Put32(file, kSblk + 0x2c, uint32_t(audioSize));
    return file;
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    std::error_code error;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return static_cast<bool>(output);
}

int MakeMus(const std::vector<std::string>& args)
{
    if (args.size() < 3)
    {
        std::fprintf(stderr, "usage: --make-mus OUT.mus IN.wav [--loop SECONDS | --loop none] [--volume 0..127]\n");
        return 2;
    }
    bool loop = true;
    double loopSeconds = 0.0;
    int volume = 80;
    for (std::size_t i = 3; i < args.size(); ++i)
    {
        if (args[i] == "--loop" && i + 1 < args.size())
        {
            const std::string value = args[++i];
            if (value == "none") loop = false;
            else loopSeconds = std::stod(value);
        }
        else if (args[i] == "--volume" && i + 1 < args.size()) volume = std::stoi(args[++i]);
        else { std::fprintf(stderr, "--make-mus: unknown option %s\n", args[i].c_str()); return 2; }
    }
    const WAVE wave = ReadWav(cane::path(args[2]));
    const double seconds = double(wave.channels[0].size()) / wave.rate;
    if (wave.channels[0].size() < 2) { std::fprintf(stderr, "--make-mus: the WAV is empty\n"); return 1; }
    if (loop && (loopSeconds < 0.0 || loopSeconds >= seconds))
    {
        std::fprintf(stderr, "--make-mus: --loop %.3f is outside the WAV (%.3f s)\n", loopSeconds, seconds);
        return 2;
    }
    std::vector<std::vector<uint8_t>> tones;
    std::size_t introSamples = 0, loopSamples = 0;
    // a mono WAV still gets a left and a right tone, so it plays at the same level as a stereo one
    for (int c = 0; c < 2; ++c)
    {
        const auto& channel = wave.channels[std::min<int>(c, int(wave.channels.size()) - 1)];
        const std::vector<float> laid = Layout(channel, wave.rate, loop, loopSeconds, introSamples, loopSamples);
        tones.push_back(EncodeAdpcm(laid, introSamples, loopSamples));
    }
    const fs::path out = cane::path(args[1]);
    if (!WriteFile(out, BuildMus(tones, volume)))
    {
        std::fprintf(stderr, "--make-mus: cannot write %s\n", args[1].c_str());
        return 1;
    }
    if (loop)
        std::printf("%s: %.3f s, %d Hz %s; loops from %.4f s (block-aligned at 48 kHz: the start moves by %.3f ms, the loop "
                    "length changes by %+.2f samples)\n",
            args[1].c_str(), seconds, wave.rate, wave.channels.size() == 1 ? "mono" : "stereo",
            double(introSamples) / kRate,
            (double(introSamples) / kRate - loopSeconds) * 1000.0,
            double(loopSamples) - (seconds - loopSeconds) * kRate);
    else
        std::printf("%s: %.3f s, %d Hz %s, played once\n", args[1].c_str(), seconds, wave.rate,
            wave.channels.size() == 1 ? "mono" : "stereo");
    return 0;
}

int ExportWav(const std::vector<std::string>& args)
{
    if (args.size() < 3)
    {
        std::fprintf(stderr, "usage: --export-wav IN.mus OUT.wav [--seconds N]\n");
        return 2;
    }
    double seconds = 120.0;
    for (std::size_t i = 3; i < args.size(); ++i)
    {
        if (args[i] == "--seconds" && i + 1 < args.size()) seconds = std::clamp(std::stod(args[++i]), 1.0, 1800.0);
        else { std::fprintf(stderr, "--export-wav: unknown option %s\n", args[i].c_str()); return 2; }
    }
    const fs::path out = cane::path(args[2]);
    std::vector<float> samples;   // interleaved stereo float at 48 kHz
#ifdef _WIN32
    // XAudio2 has no disk driver: the final output processor (spu_reverb_xapo.cpp) hands the mix over while the music
    // plays in real time, and the device gets silence. (It still needs an audio output device to run.)
    // (XAudio2 needs COM on this thread; the game gets it elsewhere, this tool runs before any of that)
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    samples.reserve(std::size_t((seconds + 2.0) * kRate) * 2);
    std::printf("playing %s for %.0f s ...\n", args[1].c_str(), seconds);
    SpuSetOutputCapture(&samples);
    const bool played = PlayMusicFileForCapture(cane::path(args[1]), seconds);
    SpuSetOutputCapture(nullptr);   // (the voices are gone after ShutdownSound)
    if (SUCCEEDED(com)) CoUninitialize();
    if (!played) return 1;
    if (samples.empty())
    {
        std::fprintf(stderr, "--export-wav: no audio was mixed (is an audio output device available?)\n");
        return 1;
    }
#else
    const fs::path raw = fs::path(out).concat(".raw");
    setenv("SDL_AUDIODRIVER", "disk", 1);
    setenv("SDL_DISKAUDIOFILE", raw.c_str(), 1);
    std::printf("playing %s for %.0f s ...\n", args[1].c_str(), seconds);
    if (!PlayMusicFileForCapture(cane::path(args[1]), seconds)) return 1;
    std::ifstream input(raw, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    input.close();
    std::error_code error;
    fs::remove(raw, error);
    // FAudio opens SDL with 32-bit float stereo at 48 kHz (the mastering voice's format)
    samples.resize(bytes.size() / 8 * 2);
    std::memcpy(samples.data(), bytes.data(), samples.size() * 4);
#endif
    const std::size_t frames = samples.size() / 2;
    std::vector<int16_t> pcm;
    pcm.reserve(frames * 2);
    std::size_t first = frames;
    for (std::size_t i = 0; i < frames * 2; ++i)
    {
        const float value = samples[i];
        if (first == frames && value != 0.0f) first = i / 2;
        pcm.push_back(int16_t(std::clamp(std::lround(value * 32767.0f), -32768l, 32767l)));
    }
    if (first == frames) first = 0;
    pcm.erase(pcm.begin(), pcm.begin() + std::ptrdiff_t(first * 2));   // (the engine's start-up silence)
    std::vector<uint8_t> wav(44);
    const uint32_t dataBytes = uint32_t(pcm.size() * 2);
    std::memcpy(wav.data(), "RIFF", 4);
    Put32(wav, 4, 36 + dataBytes);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    Put32(wav, 16, 16);
    Put16(wav, 20, 1);
    Put16(wav, 22, 2);
    Put32(wav, 24, kRate);
    Put32(wav, 28, kRate * 4);
    Put16(wav, 32, 4);
    Put16(wav, 34, 16);
    std::memcpy(wav.data() + 36, "data", 4);
    Put32(wav, 40, dataBytes);
    wav.insert(wav.end(), reinterpret_cast<const uint8_t*>(pcm.data()),
        reinterpret_cast<const uint8_t*>(pcm.data()) + dataBytes);
    if (!WriteFile(out, wav)) { std::fprintf(stderr, "--export-wav: cannot write %s\n", args[2].c_str()); return 1; }
    std::printf("%s: %.1f s, 48000 Hz stereo\n", args[2].c_str(), double(pcm.size() / 2) / kRate);
    return 0;
}
}  // namespace

int RunMusicTool(const std::vector<std::string>& args)
{
    if (args.empty()) return -1;
    try
    {
        if (args[0] == "--make-mus") return MakeMus(args);
        if (args[0] == "--export-wav") return ExportWav(args);
    }
    catch (const std::exception& exception)
    {
        std::fprintf(stderr, "%s: %s\n", args[0].c_str(), exception.what());
        return 1;
    }
    return -1;
}
