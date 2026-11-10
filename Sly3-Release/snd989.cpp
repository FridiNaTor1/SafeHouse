// 989snd on the host (see snd989.h). Adapted from Sly1-Release/sound.cpp (EFFECTBANK, the grain executor,
// the SPU2 ADSR and LFOs, the tone voices on XAudio2); the 989snd rules from the reference port's 989snd
// (Sly2-Reference/Sly2-Release/third_party/989snd: OpenGOAL's reimplementation, which the reference checked against the IRX):
// v2/v3 grain layout, MakeVolume / AdjustVolToGroup, PS1Note2Pitch / PitchBend, instance limits, AutoVol.

#include "snd989.h"

#include "audio.h"
#include "disc.h"
#include "spiop.h"
#include "wavfile.h"

// FAudio on every platform (on Windows too, over SDL2), so the mixer behaves the same everywhere.
#ifdef __SWITCH__
#include "../platform/switch/audio_switch.h"   // (the Switch's own mixer, with the same surface)
#else
#include "../platform/audio_linux.h"
#endif
#include "../platform/game_sched.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
constexpr float kSpuMaximumFrequencyRatio = 16383.0f / 4096.0f;
constexpr int kTickHz = 240;

// The sound processor has its own random sequence (Sly1-Release: independent of the game's).
int SoundRand()
{
    static std::minstd_rand s_engine(0x5317u);
    return static_cast<int>(s_engine() & 0x7FFFFFFFu);
}

template <typename T>
bool ReadBankValue(const std::vector<uint8_t>& bytes, std::size_t offset, T& value)
{
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Banks

struct TONE
{
    int8_t priority = 0;
    int8_t volume = 0;
    int8_t centerNote = 0;
    int8_t centerFine = 0;
    int16_t pan = 0;
    int8_t mapLow = 0;
    int8_t mapHigh = 0;
    int8_t pitchBendLow = 0;
    int8_t pitchBendHigh = 0;
    uint16_t adsr1 = 0;
    uint16_t adsr2 = 0;
    uint16_t flags = 0;
    uint32_t sampleOffset = 0;
};

enum GRAINK : uint32_t
{
    GRAINK_Null = 0, GRAINK_Tone = 1, GRAINK_XrefId = 2, GRAINK_XrefNum = 3, GRAINK_LfoSettings = 4,
    GRAINK_StartChildSound = 5, GRAINK_StopChildSound = 6, GRAINK_PluginMessage = 7, GRAINK_Branch = 8,
    GRAINK_Tone2 = 9, GRAINK_ControlNull = 20, GRAINK_LoopStart = 21, GRAINK_LoopEnd = 22,
    GRAINK_LoopContinue = 23, GRAINK_Stop = 24, GRAINK_RandPlay = 25, GRAINK_RandDelay = 26, GRAINK_RandPb = 27,
    GRAINK_Pb = 28, GRAINK_AddPb = 29, GRAINK_SetRegister = 30, GRAINK_SetRegisterRand = 31,
    GRAINK_IncRegister = 32, GRAINK_DecRegister = 33, GRAINK_TestRegister = 34, GRAINK_Marker = 35,
    GRAINK_GotoMarker = 36, GRAINK_GotoRandomMarker = 37, GRAINK_WaitForAllVoices = 38, GRAINK_PlayCycle = 39,
    GRAINK_AddRegister = 40, GRAINK_KeyOffVoices = 41, GRAINK_KillVoices = 42, GRAINK_OnStopMarker = 43,
    GRAINK_CopyRegister = 44,
};

struct LFOPARAMS
{
    uint8_t which = 0, target = 0, targetExtra = 0, shape = 0;
    uint16_t dutyCycle = 0, depth = 0, flags = 0, startOffset = 0;
    uint32_t stepSize = 0;
};

struct PLAYSOUNDPARAMS
{
    int32_t vol = 0;
    int32_t pan = 0;
    std::array<int8_t, 4> areg{};
    int32_t soundId = -1;
};

struct GRAIN
{
    uint32_t type = 0;
    int32_t delay = 0;
    TONE tone{};                        // TONE / TONE2
    std::array<int16_t, 4> param{};     // control grains (v2: the opcode's three s8 arguments)
    LFOPARAMS lfo{};                    // LFO_SETTINGS
    PLAYSOUNDPARAMS psp{};              // START/STOP_CHILD_SOUND, BRANCH
    int32_t randDelay = 1;              // RAND_DELAY
};

struct SOUND
{
    int8_t volume = 0;
    int8_t volumeGroup = 0;
    int16_t pan = 0;
    int8_t instanceLimit = 0;
    uint16_t flags = 0;
    std::vector<GRAIN> grains;

    bool IsLooper() const { return (flags & 1u) != 0; }
};

struct DecodedEffectSample
{
    std::vector<int16_t> pcm;
    uint32_t loopBegin = 0;
    uint32_t loopLength = 0;
    bool loop = false;
};

struct EFFECTBANK
{
    uint32_t hbank = 0;
    uint32_t isector = 0;
    std::vector<uint8_t> bytes;
    uint32_t blockOffset = 0;
    uint32_t sampleOffset = 0;
    uint32_t sampleSize = 0;
    uint32_t version = 0;
    uint32_t bankId = 0;
    std::vector<SOUND> sounds;
    bool fWavSamples = false;  // (PC) the samples from the installation's WAVs (CDisc parts "sample<offset>")
    // Decoded tones by sample offset, filled on first play (Sly1-Release).
    mutable std::unordered_map<uint32_t, std::shared_ptr<const DecodedEffectSample>> decodedSamples;
};

bool ReadTone(const std::vector<uint8_t>& b, std::size_t at, TONE& tone)
{
    return ReadBankValue(b, at + 0, tone.priority) && ReadBankValue(b, at + 1, tone.volume) &&
           ReadBankValue(b, at + 2, tone.centerNote) && ReadBankValue(b, at + 3, tone.centerFine) &&
           ReadBankValue(b, at + 4, tone.pan) && ReadBankValue(b, at + 6, tone.mapLow) &&
           ReadBankValue(b, at + 7, tone.mapHigh) && ReadBankValue(b, at + 8, tone.pitchBendLow) &&
           ReadBankValue(b, at + 9, tone.pitchBendHigh) && ReadBankValue(b, at + 10, tone.adsr1) &&
           ReadBankValue(b, at + 12, tone.adsr2) && ReadBankValue(b, at + 14, tone.flags) &&
           ReadBankValue(b, at + 16, tone.sampleOffset);
}

bool ReadLfo(const std::vector<uint8_t>& b, std::size_t at, LFOPARAMS& lfo)
{
    return ReadBankValue(b, at + 0, lfo.which) && ReadBankValue(b, at + 1, lfo.target) &&
           ReadBankValue(b, at + 2, lfo.targetExtra) && ReadBankValue(b, at + 3, lfo.shape) &&
           ReadBankValue(b, at + 4, lfo.dutyCycle) && ReadBankValue(b, at + 6, lfo.depth) &&
           ReadBankValue(b, at + 8, lfo.flags) && ReadBankValue(b, at + 10, lfo.startOffset) &&
           ReadBankValue(b, at + 12, lfo.stepSize);
}

bool ReadPlaySoundParams(const std::vector<uint8_t>& b, std::size_t at, PLAYSOUNDPARAMS& psp)
{
    return ReadBankValue(b, at + 0, psp.vol) && ReadBankValue(b, at + 4, psp.pan) &&
           ReadBankValue(b, at + 8, psp.areg) && ReadBankValue(b, at + 12, psp.soundId);
}

// The file: {type 3, 2 chunks, (offset, size) x 2}; chunk 0 the SBlk block, chunk 1 the samples.
// Version 1 grains are 0x28 bytes with their parameters inline (Sly 1); versions 2 and 3 (Sly 2) are 8 bytes,
// {s8 args[3], u8 type; s32 delay}, the larger parameters at the grain data (+0x34) + the 24-bit argument.
// orig: (IOP) 989snd's bank loader; Sly1-Release Parse989SfxBank; Sly2-Reference third_party loader.cpp
bool Parse989SfxBank(EFFECTBANK& bank)
{
    const std::vector<uint8_t>& b = bank.bytes;
    uint32_t fileType = 0, chunkCount = 0, blockSize = 0;
    if (!ReadBankValue(b, 0, fileType) || (fileType != 1 && fileType != 3) || !ReadBankValue(b, 4, chunkCount) ||
        chunkCount < 2)
        return false;
    if (!ReadBankValue(b, 8, bank.blockOffset) || !ReadBankValue(b, 12, blockSize) ||
        !ReadBankValue(b, 16, bank.sampleOffset) || !ReadBankValue(b, 20, bank.sampleSize))
        return false;
    if (bank.blockOffset > b.size() || blockSize > b.size() - bank.blockOffset || bank.sampleOffset > b.size() ||
        bank.sampleSize > b.size() - bank.sampleOffset)
        return false;

    const std::size_t blk = bank.blockOffset;
    uint32_t dataId = 0, firstSound = 0, firstGrain = 0, grainData = 0;
    uint16_t soundCount = 0;
    if (!ReadBankValue(b, blk, dataId) || std::memcmp(&dataId, "SBlk", 4) != 0 ||
        !ReadBankValue(b, blk + 4, bank.version) || !ReadBankValue(b, blk + 12, bank.bankId) ||
        !ReadBankValue(b, blk + 22, soundCount) || !ReadBankValue(b, blk + 28, firstSound) ||
        !ReadBankValue(b, blk + 32, firstGrain))
        return false;
    if (bank.version >= 2 && !ReadBankValue(b, blk + 0x34, grainData)) return false;

    constexpr std::size_t kSoundRecordSize = 12;
    bank.sounds.clear();
    bank.sounds.reserve(soundCount);
    for (uint16_t i = 0; i < soundCount; ++i)
    {
        const std::size_t record = blk + firstSound + static_cast<std::size_t>(i) * kSoundRecordSize;
        SOUND sound{};
        int8_t grainCount = 0;
        uint32_t relativeGrain = 0;
        if (!ReadBankValue(b, record + 0, sound.volume) || !ReadBankValue(b, record + 1, sound.volumeGroup) ||
            !ReadBankValue(b, record + 2, sound.pan) || !ReadBankValue(b, record + 4, grainCount) ||
            grainCount < 0 || !ReadBankValue(b, record + 5, sound.instanceLimit) ||
            !ReadBankValue(b, record + 6, sound.flags) || !ReadBankValue(b, record + 8, relativeGrain))
            return false;
        const std::size_t grainTable = blk + firstGrain + relativeGrain;
        for (int igrain = 0; igrain < grainCount; ++igrain)
        {
            GRAIN grain{};
            if (bank.version < 2)
            {
                const std::size_t at = grainTable + static_cast<std::size_t>(igrain) * 0x28;
                if (!ReadBankValue(b, at, grain.type) || !ReadBankValue(b, at + 4, grain.delay)) return false;
                const std::size_t data = at + 8;
                switch (grain.type)
                {
                case GRAINK_Tone: case GRAINK_Tone2: if (!ReadTone(b, data, grain.tone)) return false; break;
                case GRAINK_LfoSettings: if (!ReadLfo(b, data, grain.lfo)) return false; break;
                case GRAINK_Branch: case GRAINK_StartChildSound: case GRAINK_StopChildSound:
                    if (!ReadPlaySoundParams(b, data, grain.psp)) return false;
                    break;
                case GRAINK_RandDelay: if (!ReadBankValue(b, data, grain.randDelay)) return false; break;
                default: if (!ReadBankValue(b, data, grain.param)) return false; break;
                }
            }
            else
            {
                const std::size_t at = grainTable + static_cast<std::size_t>(igrain) * 8;
                uint32_t opcode = 0;
                if (!ReadBankValue(b, at, opcode) || !ReadBankValue(b, at + 4, grain.delay)) return false;
                grain.type = opcode >> 24;
                const uint32_t value = opcode & 0xFFFFFF;
                const std::size_t data = blk + grainData + value;
                switch (grain.type)
                {
                case GRAINK_Tone: case GRAINK_Tone2: if (!ReadTone(b, data, grain.tone)) return false; break;
                case GRAINK_LfoSettings: if (!ReadLfo(b, data, grain.lfo)) return false; break;
                case GRAINK_Branch: case GRAINK_StartChildSound: case GRAINK_StopChildSound:
                    if (!ReadPlaySoundParams(b, data, grain.psp)) return false;
                    break;
                case GRAINK_PluginMessage: break;
                case GRAINK_RandDelay: grain.randDelay = static_cast<int32_t>(value) + 1; break;
                default:
                    for (int k = 0; k < 3; ++k) grain.param[k] = static_cast<int8_t>((opcode >> (8 * k)) & 0xFF);
                    break;
                }
            }
            sound.grains.push_back(grain);
        }
        bank.sounds.push_back(std::move(sound));
    }
    return true;
}

// Decodes one SPU2 ADPCM sample from `begin` through its loop-end block (Sly1-Release DecodeSpuAdpcm,
// including the converged loop: the SPU2 keeps the predictor history over the loop jump).
void DecodeSpuAdpcm(const std::vector<uint8_t>& data, std::size_t begin, std::size_t end,
                    std::vector<int16_t>& pcm, bool& loop, uint32_t& loopBegin, uint32_t& loopLength)
{
    int h1 = 0, h2 = 0;
    uint32_t loopStart = 0;
    std::size_t loopBlockStart = 0, loopBlockEnd = 0;
    bool sawLoopStart = false;
    int16_t block28[28];
    for (std::size_t block = begin; block + 16 <= end; block += 16)
    {
        const uint8_t flags = data[block + 1];
        if (flags & 4)
        {
            loopStart = static_cast<uint32_t>(pcm.size());
            loopBlockStart = block;
            sawLoopStart = true;
        }
        DecodeAdpcmBlock(&data[block], block28, &h1, &h2);
        pcm.insert(pcm.end(), block28, block28 + 28);
        if (flags & 1)
        {
            loopBlockEnd = block + 16;
            loop = (flags & 2) != 0 && sawLoopStart;
            if (loop)
            {
                loopBegin = loopStart;
                loopLength = static_cast<uint32_t>(pcm.size()) - loopStart;
            }
            break;
        }
    }
    if (loop && loopBlockStart < loopBlockEnd)
    {
        std::vector<int16_t> steadyLoop;
        for (int iteration = 0; iteration < 32; ++iteration)
        {
            const int p1 = h1, p2 = h2;
            steadyLoop.clear();
            for (std::size_t block = loopBlockStart; block < loopBlockEnd; block += 16)
            {
                DecodeAdpcmBlock(&data[block], block28, &h1, &h2);
                steadyLoop.insert(steadyLoop.end(), block28, block28 + 28);
            }
            if (h1 == p1 && h2 == p2) break;
        }
        if (steadyLoop.size() == loopLength) std::copy(steadyLoop.begin(), steadyLoop.end(), pcm.begin() + loopStart);
    }
}

// orig: (IOP) 989snd (0x19C98 at the voice's start, 0x6A48 at a pitch change): a noise tone sets no pitch; the core's
// noise clock (sceSdSetCoreAttr SD_CORE_NOISE_CLK) = the tone's center note + pb * 32 / 0x7FFF, kept to 0..63
int NoiseClock(const TONE& tone, int pb) { return std::clamp(tone.centerNote + (pb << 5) / 0x7FFF, 0, 63); }

// The SPU2's noise: one generator per core, heard by every voice of the core with noise on, at the core's clock (PCSX2
// SPU2 GetNoiseValues: the LFSR steps when the counter passes 0x8000 >> (clock >> 2), the counter advancing by
// 1 + {0, 84, 140, 180} / 210 (clock & 3) a 48 kHz sample; the output holds between steps). Generated in chunks of one
// 989snd tick (200 samples) that the core's noise voices queue in turn (so they play the same signal).
constexpr int kcNoiseChunk = 200;
constexpr int kcNoiseHistory = 64;
using NOISECHUNK = std::array<int16_t, kcNoiseChunk>;
struct NOISECORE
{
    // orig: (IOP) 989snd's noise owners (0x1F3FC priority, 0x1F404 voice, 0x1F40C lock; core 1 at +4): set by the noise
    // allocation 0x19564, cleared when the voice is freed (0x1AEC0)
    int priority = -1;
    uint64_t idOwner = 0;
    bool fLock = false;
    int clock = 0x3F;
    uint16_t lfsr = 1;
    uint32_t cnt = 0;
    uint64_t serialNext = 0;   // the next chunk to generate
    std::array<std::shared_ptr<NOISECHUNK>, kcNoiseHistory> achunk{};
};
constexpr uint8_t kanoiseAdd[64] = {1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1,
                                     1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
                                     1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1};
uint16_t LfsrNext(uint16_t lfsr) { return static_cast<uint16_t>((lfsr << 1) | kanoiseAdd[(lfsr >> 10) & 63]); }
std::array<NOISECORE, 2> s_anoisecore = [] {
    std::array<NOISECORE, 2> a{};
    // (the hardware's LFSR has run since power-on: not from its first values)
    for (NOISECORE& nc : a)
        for (int i = 0; i < 4096; ++i) nc.lfsr = LfsrNext(nc.lfsr);
    return a;
}();
uint64_t s_idVoiceNext = 1;

// the core's chunk of that serial (generated up to it on demand; an old serial gives the oldest kept)
std::shared_ptr<NOISECHUNK> PchunkNoise(int core, uint64_t serial)
{
    static constexpr uint32_t freqAdd[5] = {0, 84, 140, 180, 210};
    NOISECORE& nc = s_anoisecore[size_t(core)];
    while (nc.serialNext <= serial)
    {
        auto pchunk = std::make_shared<NOISECHUNK>();
        const uint32_t level = (0x8000u >> (nc.clock >> 2)) << 16;
        for (int16_t& smp : *pchunk)
        {
            nc.cnt += 0x10000 + freqAdd[nc.clock & 3];
            if ((nc.cnt & 0xFFFF) >= freqAdd[4]) nc.cnt += 0x10000 - freqAdd[4];
            if (nc.cnt >= level)
            {
                while (nc.cnt >= level) nc.cnt -= level;
                nc.lfsr = LfsrNext(nc.lfsr);
            }
            smp = static_cast<int16_t>(nc.lfsr);
        }
        nc.achunk[nc.serialNext % kcNoiseHistory] = std::move(pchunk);
        ++nc.serialNext;
    }
    if (serial + kcNoiseHistory <= nc.serialNext) serial = nc.serialNext - kcNoiseHistory + 1;
    return nc.achunk[serial % kcNoiseHistory];
}

// (test: SLY2N_PCMTAP=dir: every effect sample as it is first played, dir/sfx-<bank id>-<offset>-<loop begin>-<loop
// length>.raw, s16 mono 48 kHz; the audio identity check)
void TapEffectSample(const EFFECTBANK& bank, uint32_t offset, const DecodedEffectSample& d)
{
    static const char* s_szTap = std::getenv("SLY2N_PCMTAP");
    if (!s_szTap) return;
    char sz[512];
    std::snprintf(sz, sizeof(sz), "%s/sfx-%u-%06X-%d-%u-%u.raw", s_szTap, bank.bankId, offset, int(d.loop), d.loopBegin,
                  d.loopLength);
    if (std::FILE* pf = std::fopen(sz, "wb"))
    {
        std::fwrite(d.pcm.data(), sizeof(int16_t), d.pcm.size(), pf);
        std::fclose(pf);
    }
}

// (PC) A sample from a WAV (the converted installation's, or a mod's): mono (more channels mixed), at the SPU2's
// 48 kHz (another rate resampled, linear), the loop from its smpl chunk. The installation's WAVs hold what
// DecodeSpuAdpcm decodes (the converter writes them with it): the same samples, loop begin and length.
bool FLoadEffectWav(const std::string& path, DecodedEffectSample& d)
{
    WAVINFO wi;
    std::vector<int16_t> a;
    std::string error;
    if (!FLoadWav(path, wi, a, &error) || wi.cframe == 0)
    {
        std::fprintf(stderr, "snd: %s: %s\n", path.c_str(), error.empty() ? "empty" : error.c_str());
        return false;
    }
    std::vector<int16_t> mono(std::size_t(wi.cframe));
    for (std::size_t i = 0; i < mono.size(); ++i)
    {
        int sum = 0;
        for (int k = 0; k < wi.cch; ++k) sum += a[i * std::size_t(wi.cch) + std::size_t(k)];
        mono[i] = int16_t(sum / wi.cch);
    }
    uint64_t iloop = wi.iframeLoop, iloopEnd = wi.CframeLooped();
    if (wi.rate != 48000)
    {
        const double r = double(wi.rate) / 48000.0;
        std::vector<int16_t> out(std::size_t(double(mono.size()) / r));
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            const double x = double(i) * r;
            const std::size_t i0 = std::min(std::size_t(x), mono.size() - 1), i1 = std::min(i0 + 1, mono.size() - 1);
            out[i] = int16_t(std::lround(mono[i0] + (mono[i1] - mono[i0]) * (x - double(i0))));
        }
        iloop = uint64_t(double(iloop) / r);
        iloopEnd = std::min<uint64_t>(uint64_t(double(iloopEnd) / r), out.size());
        mono = std::move(out);
    }
    if (mono.empty()) return false;
    d.pcm = std::move(mono);
    d.loop = wi.fLoop && iloop < iloopEnd;
    if (d.loop)
    {
        d.loopBegin = uint32_t(iloop);
        d.loopLength = uint32_t(iloopEnd - iloop);
    }
    return true;
}

std::shared_ptr<const DecodedEffectSample> EffectToneSample(const EFFECTBANK& bank, const TONE& tone)
{
    const auto it = bank.decodedSamples.find(tone.sampleOffset);
    if (it != bank.decodedSamples.end()) return it->second;
    if (bank.fWavSamples)
        if (const std::string path = g_disc.PathPart(bank.isector, "sample" + std::to_string(tone.sampleOffset)); !path.empty())
        {
            DecodedEffectSample d;
            if (FLoadEffectWav(path, d))
            {
                TapEffectSample(bank, tone.sampleOffset, d);
                auto sample = std::make_shared<const DecodedEffectSample>(std::move(d));
                bank.decodedSamples.emplace(tone.sampleOffset, sample);
                return sample;
            }
        }
    if (tone.sampleOffset >= bank.sampleSize) return nullptr;
    DecodedEffectSample d;
    DecodeSpuAdpcm(bank.bytes, std::size_t(bank.sampleOffset) + tone.sampleOffset,
                   std::size_t(bank.sampleOffset) + bank.sampleSize, d.pcm, d.loop, d.loopBegin, d.loopLength);
    if (d.pcm.empty()) return nullptr;
    TapEffectSample(bank, tone.sampleOffset, d);
    auto sample = std::make_shared<const DecodedEffectSample>(std::move(d));
    bank.decodedSamples.emplace(tone.sampleOffset, sample);
    return sample;
}

// ---------------------------------------------------------------------------------------------------------
// Volumes and pitch (989snd)

struct VOLPAIR
{
    int left = 0;
    int right = 0;
};

std::array<int, 32> s_amastervol = [] {
    std::array<int, 32> a{};
    a.fill(0x400);
    return a;
}();
std::array<int8_t, 32> s_ablockreg{};  // 989snd's global block registers (g_block_reg)
uint32_t s_grouppausemask = 0;

// 989snd's pan table: {left, right} = 0x3FFF {cos, sin} of i / 2 degrees, truncated.
VOLPAIR PanTable(int i)
{
    const double a = i * 3.14159265358979323846 / 360.0;
    return {static_cast<int>(16383.0 * std::cos(a) + 1e-9), static_cast<int>(16383.0 * std::sin(a) + 1e-9)};
}

// The groups' duck factors (16.16, 0x10000 none), set by the duckers (989snd 0x1F498).
std::array<uint32_t, 15> s_agroupduck = [] {
    std::array<uint32_t, 15> a{};
    a.fill(0x10000);
    return a;
}();

// orig: (IOP) 989snd MakeVolume (Sly 2's 989snd.irx 0x1B628): Sly 2's StartSoundSystem sets the "square the
// volumes" flag (0x1E918), so each volume factor is squared (x * x / 127) before the product; the pan law after it
// is not (OpenGOAL's 989snd, which the reference ports, squares the whole product in AdjustVolToGroup instead, which
// squares the pan law too: a centred voice at 0.5 instead of the PS2's 0.707, PCSX2-measured on the dialog).
VOLPAIR MakeVolume(int vol1, int pan1, int vol2, int pan2, int vol3, int pan3)
{
    auto sq = [](int v) { return v * v / 0x7F; };
    int vol = sq(vol1) * 258;
    vol = (vol * sq(vol2)) / 0x7F;
    vol = (vol * sq(vol3)) / 0x7F;
    if (vol == 0) return {};
    int pan = (pan1 + pan2 + pan3) % 360;
    if (pan < 0) pan += 360;
    pan = pan >= 270 ? pan - 270 : pan + 90;
    if (pan < 180)
    {
        const VOLPAIR t = PanTable(pan);
        return {t.left * vol / 0x3FFF, t.right * vol / 0x3FFF};
    }
    const VOLPAIR t = PanTable(pan - 180);
    return {t.right * vol / 0x3FFF, t.left * vol / 0x3FFF};
}

// orig: (IOP) 989snd AdjustVolToGroup (989snd.irx 0x1BD08): the group's master squared (m * m / 0x400) times its
// duck factor squared (when not 0 or unity), applied to the volume (not squared itself: MakeVolume squared its factors)
int AdjustVolToGroup(int volume, int group)
{
    if (group >= 15) return volume;
    if (volume >= 0x7FFF) volume = 0x7FFE;
    uint32_t duck = s_agroupduck[size_t(group)];
    if (duck - 1u <= 0xFFFEu) duck = (duck * duck) >> 16;
    int m2 = s_amastervol[size_t(group)] * s_amastervol[size_t(group)];
    if (m2 < 0) m2 += 0x3FF;
    m2 >>= 10;
    const int f = static_cast<int>((static_cast<uint32_t>(m2) * duck) >> 16);
    int x = volume * f;
    if (x < 0) x += 0x3FF;
    return static_cast<int16_t>(static_cast<uint32_t>(x) << 6 >> 16);
}

// 989snd's note table and sceSdNote2Pitch (libsd), as OpenGOAL's 989snd has them (Sly2-Reference third_party
// util.cpp, ISC): the SPU2 pitch, 0x1000 = one source sample per 48 kHz output sample.
const uint16_t NotePitchTable[] = {
    0x8000, 0x879C, 0x8FAC, 0x9837, 0xA145, 0xAADC, 0xB504, 0xBFC8, 0xCB2F, 0xD744, 0xE411, 0xF1A1,
    0x8000, 0x800E, 0x801D, 0x802C, 0x803B, 0x804A, 0x8058, 0x8067, 0x8076, 0x8085, 0x8094, 0x80A3,
    0x80B1, 0x80C0, 0x80CF, 0x80DE, 0x80ED, 0x80FC, 0x810B, 0x811A, 0x8129, 0x8138, 0x8146, 0x8155,
    0x8164, 0x8173, 0x8182, 0x8191, 0x81A0, 0x81AF, 0x81BE, 0x81CD, 0x81DC, 0x81EB, 0x81FA, 0x8209,
    0x8218, 0x8227, 0x8236, 0x8245, 0x8254, 0x8263, 0x8272, 0x8282, 0x8291, 0x82A0, 0x82AF, 0x82BE,
    0x82CD, 0x82DC, 0x82EB, 0x82FA, 0x830A, 0x8319, 0x8328, 0x8337, 0x8346, 0x8355, 0x8364, 0x8374,
    0x8383, 0x8392, 0x83A1, 0x83B0, 0x83C0, 0x83CF, 0x83DE, 0x83ED, 0x83FD, 0x840C, 0x841B, 0x842A,
    0x843A, 0x8449, 0x8458, 0x8468, 0x8477, 0x8486, 0x8495, 0x84A5, 0x84B4, 0x84C3, 0x84D3, 0x84E2,
    0x84F1, 0x8501, 0x8510, 0x8520, 0x852F, 0x853E, 0x854E, 0x855D, 0x856D, 0x857C, 0x858B, 0x859B,
    0x85AA, 0x85BA, 0x85C9, 0x85D9, 0x85E8, 0x85F8, 0x8607, 0x8617, 0x8626, 0x8636, 0x8645, 0x8655,
    0x8664, 0x8674, 0x8683, 0x8693, 0x86A2, 0x86B2, 0x86C1, 0x86D1, 0x86E0, 0x86F0, 0x8700, 0x870F,
    0x871F, 0x872E, 0x873E, 0x874E, 0x875D, 0x876D, 0x877D, 0x878C};

// orig: (IOP) libsd sceSdNote2Pitch
uint16_t sceSdNote2Pitch(uint16_t center_note, uint16_t center_fine, uint16_t note, int16_t fine) {
  int32_t _fine;
  int32_t _fine2;
  int32_t _note;
  int32_t offset1, offset2;
  int32_t val;
  int32_t val2;
  int32_t val3;
  int32_t ret;

  _fine = fine + (uint16_t)center_fine;
  _fine2 = _fine;

  if (_fine < 0)
    _fine2 = _fine + 127;

  _fine2 = _fine2 / 128;
  _note = note + _fine2 - center_note;
  val3 = _note / 6;

  if (_note < 0)
    val3--;

  offset2 = _fine - _fine2 * 128;

  if (_note < 0)
    val2 = -1;
  else
    val2 = 0;
  if (val3 < 0)
    val3--;

  val2 = (val3 / 2) - val2;
  val = val2 - 2;
  offset1 = _note - (val2 * 12);

  if ((offset1 < 0) || ((offset1 == 0) && (offset2 < 0))) {
    offset1 = offset1 + 12;
    val = val2 - 3;
  }

  if (offset2 < 0) {
    offset1 = (offset1 - 1) + _fine2;
    offset2 += (_fine2 + 1) * 128;
  }

  ret = (NotePitchTable[offset1] * NotePitchTable[offset2 + 12]) / 0x10000;

  if (val < 0)
    ret = (ret + (1 << (-val - 1))) >> -val;

  return (uint16_t)ret;
}

// orig: (IOP) 989snd PS1Note2Pitch: a non-negative center note is a PS1 (44.1 kHz) note.
double PitchRatio(int centerNote, int centerFine, int note, int fine)
{
    const bool ps1 = centerNote >= 0;
    if (!ps1) centerNote = -centerNote;
    uint32_t pitch = sceSdNote2Pitch(static_cast<uint16_t>(centerNote), static_cast<uint16_t>(static_cast<int8_t>(centerFine)),
                                     static_cast<uint16_t>(note), static_cast<int16_t>(fine));
    if (ps1) pitch = static_cast<uint16_t>(44100 * pitch / 48000);
    return std::min<double>(std::min<uint32_t>(pitch, 0x3FFF) / 4096.0, kSpuMaximumFrequencyRatio);
}

// orig: (IOP) 989snd PitchBend
void PitchBend(const TONE& tone, int pb, int pm, int startNote, int startFine, int* pnote, int* pfine)
{
    const int v9 = (startNote << 7) + startFine + pm;
    const int v7 = pb >= 0 ? tone.pitchBendHigh * (pb << 7) / 0x7FFF + v9 : tone.pitchBendLow * (pb << 7) / 0x8000 + v9;
    *pnote = v7 / 128;
    *pfine = v7 % 128;
}

// ---------------------------------------------------------------------------------------------------------
// Voices and sounds

enum EffectEnvelopePhase
{
    EffectEnvelope_Attack,
    EffectEnvelope_Decay,
    EffectEnvelope_Sustain,
    EffectEnvelope_Release,
    EffectEnvelope_Stopped
};

struct EFFECTVOICE
{
    IXAudio2SourceVoice* voice = nullptr;
    std::shared_ptr<const DecodedEffectSample> sample;  // held until DestroyVoice: read in place
    TONE tone{};
    int gVol = 0;  // the tone's resolved volume / pan
    int gPan = 0;
    int startNote = 60;
    int startFine = 0;
    bool loop = false;
    bool killed = false;
    uint32_t envelopeBits = 0;
    uint32_t envelopeCounter = 0;
    int envelopePhase = EffectEnvelope_Stopped;
    uint64_t id = 0;
    int spu = -1;          // its SPU2 voice (s_aspuvoice), -1 none
    int noiseCore = -1;    // a noise tone: its core (0 / 1), its next chunk and the chunks it has queued
    uint64_t serialNoise = 0;
    std::vector<std::shared_ptr<NOISECHUNK>> achunkQueued;
    int envelopeShift = 0;
    int envelopeStep = 0;
    int envelopeLevel = 0;
    int envelopeTarget = 0;
    bool envelopeExponential = false;
    bool envelopeDecreasing = false;
};

struct EFFECTLFO
{
    uint8_t type = 0;  // 0 off, 1 sine, 2 square, 3 triangle, 4 saw, 5 random
    uint8_t target = 0;  // 0 none, 1 volume, 2 pan, 3 pitch modifier, 4 pitch bend
    uint16_t setupFlags = 0;
    int depth = 0;
    uint32_t stepSize = 0;
    int32_t nextStep = 0;
    int32_t stateHold1 = 0;
    int32_t stateHold2 = 0;
    int32_t range = 0;
    uint32_t tick = 0;
};

struct BLOCKSOUND
{
    uint32_t h = 0;
    EFFECTBANK* pbank = nullptr;
    int isound = 0;      // the current sound (BRANCH changes it)
    int isoundOrig = 0;
    int group = 0;
    int origVolume = 0, origPan = 0;
    int curVolume = 0, curPan = 0, curPb = 0, curPm = 0;
    int appVolume = 0x400, appPan = 0, appPb = 0, appPm = 0;
    int lfoVolume = 0, lfoPan = 0, lfoPb = 0, lfoPm = 0;
    int note = 60, fine = 0;
    std::array<int8_t, 4> registers{};
    std::array<EFFECTLFO, 4> lfos{};
    int nextGrain = 0;
    int countdown = 0;
    bool done = false;
    bool paused = false;
    bool stopping = false;    // +0x16 flag 4: stopped once (a second stop kills, 0xEDA0)
    bool lfoStopped = false;  // 0x2440: its LFOs taken off the LFO list at a stop
    uint64_t tickStop = 0;    // (debug: when its voices were keyed off; snd_SpuStats' stale count)
    bool skipGrains = false;
    int grainsToPlay = 0, grainsToSkip = 0;
    uint64_t startTick = 0;
    int autoVolTicks = 0, autoVolTarget = 0, autoVolPos = 0, autoVolStep = 0;
    bool autoVolStop = false;
    std::vector<EFFECTVOICE> voices;
    std::vector<std::unique_ptr<BLOCKSOUND>> children;

    SOUND& Sound() const { return pbank->sounds[isound]; }
    ~BLOCKSOUND();  // (its voices destroyed: an SPU voice never outlives its owner)
};

std::map<uint32_t, std::unique_ptr<EFFECTBANK>> s_mpbank;
std::map<uint32_t, std::unique_ptr<BLOCKSOUND>> s_mpsound;
uint32_t s_hbankNext = 0;
uint32_t s_hsoundSerial = 0;
uint64_t s_tick = 0;
std::atomic<bool> s_fThread{false};
std::thread s_thread;
bool s_fStarted = false;

void ConfigureEffectEnvelope(EFFECTVOICE& v)
{
    switch (v.envelopePhase)
    {
    case EffectEnvelope_Attack:
        v.envelopeExponential = (v.envelopeBits & (1u << 15)) != 0;
        v.envelopeDecreasing = false;
        v.envelopeShift = (v.envelopeBits >> 10) & 0x1F;
        v.envelopeStep = 7 - static_cast<int>((v.envelopeBits >> 8) & 3);
        v.envelopeTarget = 0x7FFF;
        break;
    case EffectEnvelope_Decay:
        v.envelopeExponential = true;
        v.envelopeDecreasing = true;
        v.envelopeShift = (v.envelopeBits >> 4) & 0x0F;
        v.envelopeStep = -8;
        v.envelopeTarget = (static_cast<int>(v.envelopeBits & 0x0F) + 1) << 11;
        break;
    case EffectEnvelope_Sustain:
        v.envelopeExponential = (v.envelopeBits & (1u << 31)) != 0;
        v.envelopeDecreasing = (v.envelopeBits & (1u << 30)) != 0;
        v.envelopeShift = (v.envelopeBits >> 24) & 0x1F;
        v.envelopeStep = v.envelopeDecreasing ? -8 + static_cast<int>((v.envelopeBits >> 22) & 3)
                                              : 7 - static_cast<int>((v.envelopeBits >> 22) & 3);
        v.envelopeTarget = 0;
        break;
    case EffectEnvelope_Release:
        v.envelopeExponential = (v.envelopeBits & (1u << 21)) != 0;
        v.envelopeDecreasing = true;
        v.envelopeShift = (v.envelopeBits >> 16) & 0x1F;
        v.envelopeStep = -8;
        v.envelopeTarget = 0;
        break;
    default: break;
    }
}

void StartEffectEnvelope(EFFECTVOICE& v, uint16_t adsr1, uint16_t adsr2)
{
    v.envelopeBits = static_cast<uint32_t>(adsr1) | (static_cast<uint32_t>(adsr2) << 16);
    v.envelopeCounter = 0;
    v.envelopeLevel = 0;
    v.envelopePhase = EffectEnvelope_Attack;
    ConfigureEffectEnvelope(v);
}

// orig: (SPU2) key off
void ReleaseEffectEnvelope(EFFECTVOICE& v)
{
    if (v.envelopePhase == EffectEnvelope_Stopped || v.envelopePhase == EffectEnvelope_Release) return;
    v.envelopePhase = EffectEnvelope_Release;
    v.envelopeCounter = 0;
    ConfigureEffectEnvelope(v);
}

// One 48 kHz envelope step (Sly1-Release StepEffectEnvelope).
void StepEffectEnvelope(EFFECTVOICE& v)
{
    if (v.envelopePhase == EffectEnvelope_Stopped) return;
    uint32_t counterStep = 0x800000;
    const int adjustedShift = v.envelopeShift - 11;
    if (adjustedShift > 0) counterStep >>= adjustedShift;
    int sampleStep = v.envelopeStep * (1 << std::max(0, 11 - v.envelopeShift));
    if (v.envelopeExponential)
    {
        if (!v.envelopeDecreasing && v.envelopeLevel > 0x6000) counterStep >>= 2;
        if (v.envelopeDecreasing) sampleStep = (sampleStep * v.envelopeLevel) >> 15;
    }
    v.envelopeCounter += counterStep;
    if (v.envelopeCounter >= 0x800000)
    {
        v.envelopeCounter = 0;
        v.envelopeLevel = std::clamp(v.envelopeLevel + sampleStep, 0, 0x7FFF);
    }
    if (v.envelopePhase == EffectEnvelope_Sustain && v.envelopeDecreasing && v.envelopeLevel <= 0)
    {
        v.envelopePhase = EffectEnvelope_Stopped;
        return;
    }
    if (v.envelopePhase != EffectEnvelope_Sustain &&
        ((!v.envelopeDecreasing && v.envelopeLevel >= v.envelopeTarget) ||
         (v.envelopeDecreasing && v.envelopeLevel <= v.envelopeTarget)))
    {
        if (v.envelopePhase == EffectEnvelope_Attack) v.envelopePhase = EffectEnvelope_Decay;
        else if (v.envelopePhase == EffectEnvelope_Decay) v.envelopePhase = EffectEnvelope_Sustain;
        else v.envelopePhase = EffectEnvelope_Stopped;
        ConfigureEffectEnvelope(v);
    }
}

uint32_t EnvelopeCounterStep(int shift, bool exponential, bool decreasing, int level)
{
    uint32_t counterStep = 0x800000;
    const int adjustedShift = shift - 11;
    if (adjustedShift > 0) counterStep >>= adjustedShift;
    if (exponential && !decreasing && level > 0x6000) counterStep >>= 2;
    return counterStep;
}

// Steps the envelope by `samples` (Sly1-Release: the counter-only samples skipped in one go).
void AdvanceEffectEnvelope(EFFECTVOICE& v, int samples)
{
    while (samples > 0 && v.envelopePhase != EffectEnvelope_Stopped)
    {
        const int phase = v.envelopePhase;
        StepEffectEnvelope(v);
        --samples;
        if (v.envelopePhase == phase)
        {
            const uint32_t step =
                EnvelopeCounterStep(v.envelopeShift, v.envelopeExponential, v.envelopeDecreasing, v.envelopeLevel);
            if (step != 0 && v.envelopeCounter < 0x800000)
            {
                const uint32_t quiet = std::min((0x7FFFFFu - v.envelopeCounter) / step, static_cast<uint32_t>(samples));
                v.envelopeCounter += quiet * step;
                samples -= static_cast<int>(quiet);
            }
        }
    }
}

// orig: (IOP) 989snd 0x1AEC0: a freed voice that owns its core's noise gives it up (priority -1, unlocked)
void ReleaseNoiseCore(const EFFECTVOICE& v)
{
    if (v.noiseCore < 0) return;
    NOISECORE& nc = s_anoisecore[size_t(v.noiseCore)];
    if (nc.idOwner != v.id) return;
    nc.idOwner = 0;
    nc.priority = -1;
    nc.fLock = false;
}

// a noise voice's queue kept 3 chunks deep (15 ms) with its core's next chunks
void FeedNoiseVoice(EFFECTVOICE& v)
{
    if (!v.voice || v.noiseCore < 0) return;
    XAUDIO2_VOICE_STATE state{};
    v.voice->GetState(&state);
    // (the chunks played are let go: the queue holds the last BuffersQueued submitted)
    while (v.achunkQueued.size() > state.BuffersQueued) v.achunkQueued.erase(v.achunkQueued.begin());
    for (UINT32 c = state.BuffersQueued; c < 3; ++c)
    {
        std::shared_ptr<NOISECHUNK> pchunk = PchunkNoise(v.noiseCore, v.serialNoise++);
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(sizeof(NOISECHUNK));
        buffer.pAudioData = reinterpret_cast<const BYTE*>(pchunk->data());
        if (FAILED(v.voice->SubmitSourceBuffer(&buffer))) break;
        v.achunkQueued.push_back(std::move(pchunk));
    }
}

// orig: (IOP) 989snd's voice table (0x263A8: 48 records of 0x38 bytes; +0x08 status, +0x0C / +0x10 the owner and its
// callback, +0x1A priority, +0x1B group, +0x1C start time, +0x28 lock): the SPU2's 48 voices, 24 a core. Every tone
// and every stream channel takes one (0x19750); a voice is free again once its envelope reads 0 (0x10464 -> 0x1A184).
constexpr int kcSpuVoice = 48;
enum SPUVS { SPUVS_Free = 0, SPUVS_Effect = 2, SPUVS_Reserved = 3, SPUVS_Stream = 4 };
struct SPUVOICE
{
    int status = SPUVS_Free;
    uint8_t priority = 0;  // (a byte, compared unsigned)
    bool fLock = false;
    uint64_t tickStart = 0;
    BLOCKSOUND* pbs = nullptr;  // an effect voice's owner and its EFFECTVOICE id
    uint64_t idVoice = 0;
};
std::array<SPUVOICE, kcSpuVoice> s_aspuvoice{};
struct SPUSTATS
{
    uint64_t cAlloc = 0, cSteal = 0, cFail = 0;
    int cPeak = 0;
};
SPUSTATS s_spustats{};
// (debug: SLY2N_NOVOICELIMIT=1 plays every tone, as before the allocator: for comparisons only)
bool FVoiceLimit()
{
    static const bool s_f = std::getenv("SLY2N_NOVOICELIMIT") == nullptr;
    return s_f;
}

void FreeSpuVoice(int iv)
{
    if (iv < 0 || iv >= kcSpuVoice) return;
    s_aspuvoice[size_t(iv)] = SPUVOICE{};
}

int CSpuVoicesUsed()
{
    int c = 0;
    for (const SPUVOICE& sv : s_aspuvoice) c += sv.status != SPUVS_Free;
    return c;
}

uint64_t s_cHostVoiceCreated = 0, s_cHostVoiceDestroyed = 0;
double s_msTickLateMax = 0.0, s_msTickLockMax = 0.0;   // (debug: SNDCOUNT's tick timing, reset at each print)
uint64_t s_cTickLate = 0;   // (debug: SNDCOUNT's host voice balance)

void DestroyEffectVoice(EFFECTVOICE& v)
{
    if (v.spu >= 0 && s_aspuvoice[size_t(v.spu)].idVoice == v.id) FreeSpuVoice(v.spu);
    v.spu = -1;
    if (!v.voice) return;
    v.voice->Stop();
    v.voice->DestroyVoice();
    ++s_cHostVoiceDestroyed;
    v.voice = nullptr;
    v.sample.reset();
    v.achunkQueued.clear();
    ReleaseNoiseCore(v);
    v.envelopePhase = EffectEnvelope_Stopped;
}

bool FGroupPaused(int group) { return group >= 0 && group < 32 && (s_grouppausemask & (1u << group)) != 0; }

// (its voices still playing: a stolen one stays in the list until the sound's next tick)
int CLiveVoices(const BLOCKSOUND& bs)
{
    int c = 0;
    for (const EFFECTVOICE& v : bs.voices) c += v.voice != nullptr;
    return c;
}

BLOCKSOUND::~BLOCKSOUND()
{
    for (EFFECTVOICE& v : voices) DestroyEffectVoice(v);
}

// A stolen voice: its owner told (the callback +0x10 with 1: the voice off the owner's mask, 0x6B44), its noise given
// up (0x1AEC0); the SPU2 voice is silenced at once (0x19458: volume 0) for the new tone's key on.
void StealSpuVoice(int iv)
{
    SPUVOICE& sv = s_aspuvoice[size_t(iv)];
    if (sv.pbs)
        for (EFFECTVOICE& v : sv.pbs->voices)
            if (v.id == sv.idVoice)
            {
                DestroyEffectVoice(v);  // (the record is erased at the owner's next tick: CleanEffectVoices)
                break;
            }
    FreeSpuVoice(iv);
    ++s_spustats.cSteal;
}

// orig: (IOP) 989snd.irx 0x19750 (group, priority, lock): the first free voice of the group's range (0x1F3B8: every
// group 0..47, Sly 2 never narrows them: RPC 0x0D / 0x4E unused), else the range's lowest priority voice (the oldest
// of equals) that is not locked or reserved is stolen, when the new tone's priority is at least its and it is below
// 0x7E; else none (-1: the tone is not played)
int AllocSpuVoice(int first, int last, int priority, bool fLock, int status)
{
    auto claim = [&](int iv) {
        SPUVOICE& sv = s_aspuvoice[size_t(iv)];
        sv = SPUVOICE{};
        sv.status = status;
        sv.priority = static_cast<uint8_t>(priority);
        sv.fLock = fLock;
        sv.tickStart = s_tick;  // 0x10638
        ++s_spustats.cAlloc;
        s_spustats.cPeak = std::max(s_spustats.cPeak, CSpuVoicesUsed());
        return iv;
    };
    for (int iv = first; iv <= last; ++iv)
        if (s_aspuvoice[size_t(iv)].status == SPUVS_Free) return claim(iv);
    int ivLow = first;
    for (int iv = first + 1; iv <= last; ++iv)
    {
        const SPUVOICE& sv = s_aspuvoice[size_t(iv)];
        if (sv.status == SPUVS_Reserved || sv.fLock) continue;
        const SPUVOICE& low = s_aspuvoice[size_t(ivLow)];
        if (sv.priority < low.priority || (sv.priority == low.priority && sv.tickStart < low.tickStart)) ivLow = iv;
    }
    const SPUVOICE& low = s_aspuvoice[size_t(ivLow)];
    if (low.fLock || priority < low.priority || low.priority >= 0x7E || low.status == SPUVS_Reserved)
    {
        ++s_spustats.cFail;
        return -1;
    }
    StealSpuVoice(ivLow);
    return claim(ivLow);
}

// orig: (IOP) 989snd.irx 0x6D2C: the tone's priority (its byte 0); flags 2 / 4 make it fall with the sound's volume
// (+0x3A, 1024ths): mode 1 by up to a fifth, 2 a half, 3 all of it, below 951
int TonePriority(const BLOCKSOUND& bs, const TONE& tone)
{
    const int pri = tone.priority;
    if (!(tone.flags & 6)) return pri;
    const int mode = (tone.flags >> 1) & 3;
    const int range = mode == 1 ? pri / 5 : mode == 2 ? pri / 2 : pri;
    if (range == 0 || bs.appVolume >= 0x3B7) return pri;
    return static_cast<int8_t>(pri - (range - range * bs.appVolume / 950));
}

// The voice's levels: 989snd's register volume (MakeVolume -> AdjustVolToGroup -> >> 1) times the envelope.
void ApplyVoiceVolume(const BLOCKSOUND& bs, EFFECTVOICE& v)
{
    if (!v.voice) return;
    float gl = 0.0f, gr = 0.0f;
    if (!bs.paused && !v.killed && !FGroupPaused(bs.group))
    {
        const VOLPAIR p = MakeVolume(127, 0, bs.curVolume, bs.curPan, v.gVol, v.gPan);
        const int l = AdjustVolToGroup(p.left, bs.group) >> 1, r = AdjustVolToGroup(p.right, bs.group) >> 1;
        const float env = static_cast<float>(v.envelopeLevel) / 32767.0f;
        gl = static_cast<float>(l) / 16383.0f * env;
        gr = static_cast<float>(r) / 16383.0f * env;
        // (debug: SLY2N_SNDSOLO=a,b,.. keeps only those sounds audible, SLY2N_SNDMUTE=a,b,.. silences them)
        static const char* s_szSolo = std::getenv("SLY2N_SNDSOLO");
        static const char* s_szMute = std::getenv("SLY2N_SNDMUTE");
        auto fIn = [&](const char* sz) {
            for (const char* pch = sz; pch && *pch;)
            {
                if (std::atoi(pch) == bs.isoundOrig) return true;
                pch = std::strchr(pch, ',');
                if (pch) ++pch;
            }
            return false;
        };
        if ((s_szSolo && !fIn(s_szSolo)) || (s_szMute && fIn(s_szMute))) gl = gr = 0.0f;
    }
    SetVoiceLevels(v.voice, 1, gl, gr);
}

void ApplyVoicePitch(const BLOCKSOUND& bs, EFFECTVOICE& v)
{
    if (!v.voice) return;
    if (v.noiseCore >= 0)
    {
        // a noise voice has no pitch: the pitch bend moves its core's noise clock (989snd 0x6A48)
        v.voice->SetFrequencyRatio(1.0f);
        s_anoisecore[size_t(v.noiseCore)].clock = NoiseClock(v.tone, bs.curPb);
        return;
    }
    int note = 0, fine = 0;
    PitchBend(v.tone, bs.curPb, bs.curPm, v.startNote, v.startFine, &note, &fine);
    const float ratio = static_cast<float>(PitchRatio(v.tone.centerNote, v.tone.centerFine, note, fine));
    v.voice->SetFrequencyRatio(std::clamp(ratio, XAUDIO2_MIN_FREQ_RATIO, kSpuMaximumFrequencyRatio));
}

// orig: (IOP) 989snd BlockSoundHandler::SetVolPan
void SetVolPan(BLOCKSOUND& bs, int vol, int pan)
{
    if (vol >= 0)
    {
        if (vol != SND_VOLUME_DONT_CHANGE) bs.appVolume = vol;
    }
    else
        bs.appVolume = -1024 * vol / 127;
    if (pan == SND_PAN_RESET) bs.appPan = bs.Sound().pan;
    else if (pan != SND_PAN_DONT_CHANGE) bs.appPan = pan;
    bs.curVolume = std::clamp(((bs.appVolume * bs.origVolume) >> 10) + bs.lfoVolume, 0, 127);
    int p = (bs.appPan + bs.lfoPan) % 360;
    if (p < 0) p += 360;
    bs.curPan = p;
    for (auto& child : bs.children) SetVolPan(*child, bs.appVolume * bs.origVolume / 127, pan);
    for (EFFECTVOICE& v : bs.voices) ApplyVoiceVolume(bs, v);
}

// orig: (IOP) 989snd BlockSoundHandler::UpdatePitch
void UpdatePitch(BLOCKSOUND& bs)
{
    bs.curPm = bs.appPm + bs.lfoPm;
    bs.curPb = std::clamp(bs.appPb + bs.lfoPb, -32768, 32767);
    for (EFFECTVOICE& v : bs.voices) ApplyVoicePitch(bs, v);
}

void SetPBend(BLOCKSOUND& bs, int pb)
{
    for (auto& child : bs.children) SetPBend(*child, pb);
    bs.appPb = pb;
    UpdatePitch(bs);
}

void SetPMod(BLOCKSOUND& bs, int pm)
{
    for (auto& child : bs.children) SetPMod(*child, pm);
    bs.appPm = pm;
    UpdatePitch(bs);
}

int ResolveRegister(const BLOCKSOUND& bs, int value)
{
    if (value >= -4) return bs.registers[-value - 1];
    if (value == -5) return SoundRand() % 0x7F;
    const int i = -value - 6;
    return i < static_cast<int>(s_ablockreg.size()) ? s_ablockreg[i] : 0;
}

// orig: (IOP) 989snd snd_SFX_GRAIN_TYPE_TONE; Sly1-Release PlayEffectTone (the XAudio2 voice)
// (debug: noise tones not played since the last SLY2N_SNDCOUNT line, by bank sound (k: another bank, -1 - isound))
std::map<int, int> s_mpnoisedenied;
const EFFECTBANK* PbankFirstLoaded() { return s_mpbank.empty() ? nullptr : s_mpbank.begin()->second.get(); }

void PlayEffectTone(BLOCKSOUND& bs, const TONE& tone)
{
    bs.curVolume = std::clamp(((bs.appVolume * bs.origVolume) >> 10) + bs.lfoVolume, 0, 127);
    int p = (bs.appPan + bs.lfoPan) % 360;
    bs.curPan = p < 0 ? p + 360 : p;
    if (!FEnsureAudio()) return;
    const bool fNoise = (tone.flags & 8) != 0;
    const std::shared_ptr<const DecodedEffectSample> decoded = fNoise ? nullptr : EffectToneSample(*bs.pbank, tone);
    if (!fNoise && !decoded) return;
    // orig: (IOP) 989snd 0x4B44..0x4B98: the tone's priority (0x6D2C), then a voice of the sound's group (0x19750); a
    // noise tone (0x19564) needs a core's noise too: core 0 (a voice of 0..23), else core 1 (24..47), when the core's
    // noise owner is not locked (tone flag 0x20) and has a lower priority. No voice: the tone is not played.
    const int priority = TonePriority(bs, tone);
    const bool fLock = (tone.flags & 0x20) != 0;
    int core = -1, spu = -1;
    for (int c = 0; fNoise && c < 2 && core < 0; ++c)
    {
        const NOISECORE& nc = s_anoisecore[size_t(c)];
        if (nc.fLock || !(nc.priority < priority)) continue;
        if (FVoiceLimit()) spu = c == 0 ? AllocSpuVoice(0, 23, priority, fLock, SPUVS_Effect) : AllocSpuVoice(24, 47, priority, fLock, SPUVS_Effect);
        if (spu >= 0 || !FVoiceLimit()) core = c;
    }
    if (fNoise && core < 0)
    {
        ++s_mpnoisedenied[bs.pbank == PbankFirstLoaded() ? bs.isoundOrig : -1 - bs.isoundOrig];   // (debug: SLY2N_SNDCOUNT's noise denied)
        return;
    }
    if (!fNoise && FVoiceLimit() && (spu = AllocSpuVoice(0, kcSpuVoice - 1, priority, fLock, SPUVS_Effect)) < 0) return;

    EFFECTVOICE v;
    v.id = s_idVoiceNext++;
    if (spu >= 0)
    {
        v.spu = spu;
        s_aspuvoice[size_t(spu)].pbs = &bs;
        s_aspuvoice[size_t(spu)].idVoice = v.id;
    }
    v.sample = decoded;
    v.tone = tone;
    v.loop = fNoise || decoded->loop;
    int vol = tone.volume;
    if (vol < 0) vol = ResolveRegister(bs, vol);
    v.gVol = std::max(vol, 0);
    int pan = tone.pan;
    if (pan < 0)
    {
        if (pan >= -4) pan = 360 * bs.registers[-pan - 1] / 127;
        else if (pan == -5) pan = SoundRand() % 360;
        else pan = 360 * s_ablockreg[std::min(-pan - 6, 31)] / 127;
    }
    pan %= 360;
    v.gPan = pan < 0 ? pan + 360 : pan;
    v.startNote = bs.note;
    v.startFine = bs.fine;
    if (static const char* s_sz = std::getenv("SLY2N_SNDPARAMS"); s_sz && (std::atoi(s_sz) == bs.isoundOrig || std::atoi(s_sz) == -1))
        std::printf("sndtone: sound %d tone flags 0x%X center %d/%d note %d/%d pb %d pm %d vol %d adsr %04X %04X sample 0x%X\n", bs.isoundOrig,
                    tone.flags, tone.centerNote, tone.centerFine, bs.note, bs.fine, bs.curPb, bs.curPm, v.gVol, tone.adsr1, tone.adsr2,
                    tone.sampleOffset);

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    if (SUCCEEDED(g_pxaudio2->CreateSourceVoice(&v.voice, &format, 0, kSpuMaximumFrequencyRatio)) && v.voice) ++s_cHostVoiceCreated;
    else
    {
        v.voice = nullptr;
        DestroyEffectVoice(v);
        return;
    }
    if (fNoise)
    {
        // the core's noise taken (0x19564: its priority, the voice, the lock), its clock set (0x19C98)
        NOISECORE& nc = s_anoisecore[size_t(core)];
        nc.priority = tone.priority;
        nc.idOwner = v.id;
        nc.fLock = (tone.flags & 0x20) != 0;
        v.noiseCore = core;
        v.serialNoise = nc.serialNext;
        StartEffectEnvelope(v, tone.adsr1, tone.adsr2);
        ApplyVoiceVolume(bs, v);
        ApplyVoicePitch(bs, v);
        FeedNoiseVoice(v);
        if (FAILED(v.voice->Start()))
        {
            DestroyEffectVoice(v);
            return;
        }
        bs.voices.push_back(std::move(v));
        return;
    }
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(decoded->pcm.size() * sizeof(int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(decoded->pcm.data());
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    if (decoded->loop)
    {
        buffer.LoopBegin = decoded->loopBegin;
        buffer.LoopLength = decoded->loopLength;
        buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
    }
    StartEffectEnvelope(v, tone.adsr1, tone.adsr2);
    ApplyVoiceVolume(bs, v);
    ApplyVoicePitch(bs, v);
    if (FAILED(v.voice->SubmitSourceBuffer(&buffer)) || FAILED(v.voice->Start()))
    {
        DestroyEffectVoice(v);
        return;
    }
    bs.voices.push_back(std::move(v));
}

int EffectLfoValue(EFFECTLFO& lfo)
{
    const int step = lfo.nextStep >> 16;
    lfo.nextStep += static_cast<int32_t>(2u * lfo.stepSize);
    if (lfo.nextStep > 0x07FFFFFF) lfo.nextStep -= 0x08000000;
    int value = 0;
    switch (lfo.type)
    {
    case 1: value = static_cast<int>(std::sin(step * 3.14159265358979323846 / 1024.0) * 32767.0); break;
    case 2: value = step >= lfo.stateHold1 ? -32767 : 32767; break;
    case 3:
        if (step < 512) value = 32767 * step / 512;
        else if (step >= 1536) value = 32767 * (step - 1536) / 512 - 32767;
        else value = 32767 - 65534 * (step - 512) / 1024;
        break;
    case 4: value = step >= 1024 ? 32767 * (step - 1024) / 1024 - 32767 : 32767 * step / 1023; break;
    case 5:
        if (step >= 1024 && lfo.stateHold2 == 1)
        {
            lfo.stateHold2 = 0;
            lfo.stateHold1 = 2 * ((SoundRand() & 0x7FFF) - 0x3FFF);
        }
        else if (step < 1024 && lfo.stateHold2 == 0)
        {
            lfo.stateHold2 = 1;
            lfo.stateHold1 = -(SoundRand() & 0x7FFF) * (SoundRand() & 1);
        }
        value = lfo.stateHold1;
        break;
    default: break;
    }
    return (lfo.setupFlags & 1) ? -value : value;
}

// orig: (IOP) 989snd LFOTracker::Tick (every other 240 Hz tick)
void TickEffectLfos(BLOCKSOUND& bs)
{
    if (bs.lfoStopped) return;
    for (EFFECTLFO& lfo : bs.lfos)
    {
        if (lfo.target == 0 || (++lfo.tick & 1) == 0) continue;
        const int value = EffectLfoValue(lfo);
        switch (lfo.target)
        {
        case 1:
        {
            const int vol = (lfo.range * (value - 0x7FFF)) >> 16;
            if (bs.lfoVolume != vol)
            {
                bs.lfoVolume = vol;
                SetVolPan(bs, SND_VOLUME_DONT_CHANGE, SND_PAN_DONT_CHANGE);
            }
            break;
        }
        case 2:
        {
            const int pan = (lfo.range * value) >> 15;
            if (bs.lfoPan != pan)
            {
                bs.lfoPan = pan;
                SetVolPan(bs, SND_VOLUME_DONT_CHANGE, SND_PAN_DONT_CHANGE);
            }
            break;
        }
        case 3:
        {
            const int pm = (value * lfo.range) >> 15;
            if (bs.lfoPm != pm)
            {
                bs.lfoPm = pm;
                UpdatePitch(bs);
            }
            break;
        }
        case 4:
        {
            const int pb = (value * lfo.range) >> 15;
            if (bs.lfoPb != pb)
            {
                bs.lfoPb = pb;
                UpdatePitch(bs);
            }
            break;
        }
        default: break;
        }
    }
}

// orig: (IOP) 989snd snd_SFX_GRAIN_TYPE_LFO_SETTINGS + LFOTracker::Init
void ConfigureEffectLfo(BLOCKSOUND& bs, const LFOPARAMS& p)
{
    if (p.which >= bs.lfos.size()) return;
    EFFECTLFO& lfo = bs.lfos[p.which];
    lfo.target = p.target;
    if (lfo.target == 0)
    {
        lfo.type = 0;
        return;
    }
    lfo.type = p.shape;
    lfo.setupFlags = p.flags;
    lfo.depth = p.depth;
    lfo.stepSize = p.stepSize;
    lfo.stateHold1 = lfo.type == 2 ? p.dutyCycle : 0;
    lfo.stateHold2 = 0;
    lfo.nextStep = (p.flags & 2) ? (SoundRand() & 0x7FF) << 16 : p.startOffset << 16;
    if (lfo.type == 5)
    {
        lfo.stateHold1 = -(SoundRand() & 0x7FFF) * (SoundRand() & 1);
        lfo.stateHold2 = 1;
    }
    switch (lfo.target)
    {
    case 1: lfo.range = (bs.Sound().volume * lfo.depth) >> 10; break;
    case 2: lfo.range = (180 * lfo.depth) >> 10; break;
    case 3: lfo.range = (6096 * lfo.depth) >> 10; break;
    case 4: lfo.range = (0x7FFF * lfo.depth) >> 10; break;
    default: lfo.range = 0; break;
    }
    TickEffectLfos(bs);
}

std::unique_ptr<BLOCKSOUND> PbsNew(EFFECTBANK* pbank, int isound, int sfxVol, int sfxPan, const SNDPLAYPARAMS& params,
                                   uint64_t startTick);

// orig: (IOP) 989snd snd_SFX_GRAIN_TYPE_STARTCHILDSOUND; Sly1-Release StartEffectChild
void StartEffectChild(BLOCKSOUND& parent, const PLAYSOUNDPARAMS& psp)
{
    int vol = psp.vol;
    if (vol < 0) vol = ResolveRegister(parent, vol);
    vol = std::clamp(std::abs(vol), 0, 127);
    int pan = psp.pan;
    if (pan < 0)
    {
        if (pan >= -4) pan = 360 * std::min(std::abs(int(parent.registers[-pan - 1])), 127) / 127;
        else if (pan == -5) pan = SoundRand() % 360;
        else pan = 360 * std::min(std::abs(int(s_ablockreg[std::min(-pan - 6, 31)])), 127) / 127;
    }
    if (psp.soundId < 0 || psp.soundId >= static_cast<int>(parent.pbank->sounds.size())) return;
    SNDPLAYPARAMS params;
    params.mask = SNDPPM_Vol | SNDPPM_Pan | SNDPPM_PitchMod | SNDPPM_PitchBend | SNDPPM_Registers;
    params.vol = parent.appVolume * parent.origVolume / 127;
    params.pan = parent.appPan;
    params.pm = parent.appPm;
    params.pb = parent.appPb;
    params.areg = parent.registers;
    auto child = PbsNew(parent.pbank, psp.soundId, vol, pan, params, parent.startTick);
    if (child) parent.children.push_back(std::move(child));
}

void ExecuteEffectGrain(BLOCKSOUND& bs);
void StopBlockSound(BLOCKSOUND& bs);

// One grain (989snd's grain functions; Sly1-Release ExecuteEffectGrain), then the next one's countdown.
void ExecuteEffectGrain(BLOCKSOUND& bs)
{
    SOUND& sound = bs.Sound();
    if (bs.nextGrain < 0 || bs.nextGrain >= static_cast<int>(sound.grains.size()))
    {
        bs.done = true;
        return;
    }
    GRAIN& grain = sound.grains[bs.nextGrain];
    int ret = 0;
    auto reg = [&](int r) -> int8_t& { return r < 0 ? s_ablockreg[std::min(-r - 1, 31)] : bs.registers[r & 3]; };
    switch (grain.type)
    {
    case GRAINK_Tone:
    case GRAINK_Tone2: PlayEffectTone(bs, grain.tone); break;
    case GRAINK_LfoSettings: ConfigureEffectLfo(bs, grain.lfo); break;
    case GRAINK_StartChildSound: StartEffectChild(bs, grain.psp); break;
    case GRAINK_StopChildSound:
        // orig: (IOP) 989snd.irx 0x6118 -> 0xF00C -> 0xEDA0: each child of that sound stopped (its voices keyed off;
        // it is freed once they end), not dropped with its voices still playing
        if (grain.psp.soundId >= 0)
            for (auto& c : bs.children)
                if (c->isound == grain.psp.soundId) StopBlockSound(*c);
        break;
    case GRAINK_PluginMessage: break;  // (Sly 2's spiop is driven from the EE; no bank sends it messages)
    case GRAINK_Branch:
    {
        const int index = grain.psp.soundId;
        if (index < 0 || index >= static_cast<int>(bs.pbank->sounds.size())) break;
        for (EFFECTVOICE& v : bs.voices)
        {
            ReleaseEffectEnvelope(v);
            v.killed = true;
            ApplyVoiceVolume(bs, v);
        }
        SOUND& next = bs.pbank->sounds[index];
        bs.isound = index;
        bs.origVolume = next.volume;
        bs.origPan = next.pan;
        bs.curVolume = (bs.appVolume * next.volume) >> 10;
        bs.group = next.volumeGroup;
        bs.countdown = next.grains.empty() ? 0 : next.grains[0].delay;
        bs.nextGrain = -1;
        bs.skipGrains = false;
        bs.grainsToPlay = bs.grainsToSkip = 0;
        if (next.grains.empty()) bs.done = true;
        break;
    }
    case GRAINK_LoopEnd:
        for (int i = bs.nextGrain - 1; i >= 0; --i)
            if (sound.grains[i].type == GRAINK_LoopStart)
            {
                bs.nextGrain = i - 1;
                break;
            }
        break;
    case GRAINK_LoopContinue:
        for (int i = bs.nextGrain + 1; i < static_cast<int>(sound.grains.size()); ++i)
            if (sound.grains[i].type == GRAINK_LoopEnd)
            {
                bs.nextGrain = i;
                break;
            }
        break;
    case GRAINK_Stop:
        // orig: (IOP) 989snd.irx 0x51D0 returns -1: the grain step (0x626C) returns 1, the sound's tick (0x47A4) too,
        // and 0xF1F4 stops the sound (0xEDA0): its voices keyed off (a looping tone was held forever), its children
        // stopped, its LFOs stopped; or its on-stop grains run
        StopBlockSound(bs);
        return;
    case GRAINK_RandPlay:
    {
        const int options = grain.param[0], count = grain.param[1];
        if (options <= 0) break;
        int rnd = SoundRand() % options;
        if (rnd == grain.param[2] && ++rnd >= options) rnd = 0;
        grain.param[2] = static_cast<int16_t>(rnd);
        bs.nextGrain += rnd * count;
        bs.grainsToPlay = count + 1;
        bs.grainsToSkip = (options - 1 - rnd) * count;
        bs.skipGrains = true;
        break;
    }
    case GRAINK_RandDelay: ret = grain.randDelay > 0 ? SoundRand() % grain.randDelay : 0; break;
    case GRAINK_RandPb:
        SetPBend(bs, grain.param[0] * ((0xFFFF * (SoundRand() % 0x7FFF)) / 0x7FFF - 0x8000) / 100);
        break;
    case GRAINK_Pb:
        SetPBend(bs, grain.param[0] >= 0 ? 0x7FFF * grain.param[0] / 127 : -0x8000 * grain.param[0] / -128);
        break;
    case GRAINK_AddPb: SetPBend(bs, std::clamp(bs.curPb + 0x7FFF * grain.param[0] / 127, -32768, 32767)); break;
    case GRAINK_SetRegister: reg(grain.param[0]) = static_cast<int8_t>(grain.param[1]); break;
    case GRAINK_SetRegisterRand:
    {
        const int range = grain.param[2] - grain.param[1] + 1;
        if (range > 0) reg(grain.param[0]) = static_cast<int8_t>(SoundRand() % range + grain.param[1]);
        break;
    }
    case GRAINK_IncRegister: reg(grain.param[0]) = static_cast<int8_t>(std::clamp(reg(grain.param[0]) + 1, -128, 127)); break;
    case GRAINK_DecRegister: reg(grain.param[0]) = static_cast<int8_t>(std::clamp(reg(grain.param[0]) - 1, -128, 127)); break;
    case GRAINK_TestRegister:
    {
        const int value = reg(grain.param[0]), action = grain.param[1], cmp = grain.param[2];
        if ((action == 0 && value >= cmp) || (action == 1 && value != cmp) || (action >= 2 && cmp >= value))
            bs.nextGrain++;
        break;
    }
    case GRAINK_GotoMarker:
    case GRAINK_GotoRandomMarker:
    {
        int mark = grain.param[0];
        if (grain.type == GRAINK_GotoRandomMarker)
        {
            const int range = grain.param[1] - grain.param[0] + 1;
            mark = range > 0 ? SoundRand() % range + grain.param[0] : grain.param[0];
        }
        for (int i = 0; i < static_cast<int>(sound.grains.size()); ++i)
            if (sound.grains[i].type == GRAINK_Marker && sound.grains[i].param[0] == mark)
            {
                bs.nextGrain = i - 1;
                break;
            }
        break;
    }
    case GRAINK_WaitForAllVoices:
        if (!bs.voices.empty())
        {
            bs.nextGrain--;
            ret = 1;
        }
        break;
    case GRAINK_PlayCycle:
    {
        const int size = grain.param[0], count = grain.param[1];
        int index = grain.param[2];
        const int a = index++;
        if (index == size) index = 0;
        grain.param[2] = static_cast<int16_t>(index);
        bs.nextGrain += count * a;
        bs.grainsToPlay = count + 1;
        bs.grainsToSkip = (size - 1 - a) * count;
        bs.skipGrains = true;
        break;
    }
    case GRAINK_AddRegister:
        reg(grain.param[1]) = static_cast<int8_t>(std::clamp(reg(grain.param[1]) + grain.param[0], -128, 127));
        break;
    case GRAINK_KeyOffVoices:
        for (EFFECTVOICE& v : bs.voices) ReleaseEffectEnvelope(v);
        break;
    case GRAINK_KillVoices:
        for (EFFECTVOICE& v : bs.voices)
        {
            ReleaseEffectEnvelope(v);
            v.killed = true;
            ApplyVoiceVolume(bs, v);
        }
        break;
    case GRAINK_OnStopMarker: bs.nextGrain = static_cast<int>(sound.grains.size()) - 1; break;
    case GRAINK_CopyRegister: reg(grain.param[1]) = reg(grain.param[0]); break;
    default: break;  // NULL, XREF, CONTROL_NULL, LOOP_START, MARKER
    }

    SOUND& now = bs.Sound();  // (BRANCH may have changed the sound)
    if (bs.skipGrains && --bs.grainsToPlay == 0)
    {
        bs.nextGrain += bs.grainsToSkip;
        bs.skipGrains = false;
    }
    bs.nextGrain++;
    if (bs.nextGrain >= static_cast<int>(now.grains.size()))
    {
        bs.done = true;
        return;
    }
    bs.countdown = now.grains[bs.nextGrain].delay + ret;
}

// orig: (IOP) 989snd BlockSoundHandler's constructor
std::unique_ptr<BLOCKSOUND> PbsNew(EFFECTBANK* pbank, int isound, int sfxVol, int sfxPan, const SNDPLAYPARAMS& params,
                                   uint64_t startTick)
{
    SOUND& sfx = pbank->sounds[isound];
    if (sfx.grains.empty()) return nullptr;
    auto bs = std::make_unique<BLOCKSOUND>();
    bs->pbank = pbank;
    bs->isound = bs->isoundOrig = isound;
    bs->group = sfx.volumeGroup;
    bs->startTick = startTick;
    if (sfxVol == -1) sfxVol = sfx.volume;
    if (sfxPan == -1) sfxPan = sfx.pan;
    int vol = (params.mask & SNDPPM_Vol) ? params.vol : 1024;
    int pan = (params.mask & SNDPPM_Pan) ? params.pan : -1;
    if (vol == SND_VOLUME_DONT_CHANGE) vol = 1024;
    if (pan == SND_PAN_RESET || pan == SND_PAN_DONT_CHANGE) pan = sfxPan;
    bs->origVolume = sfxVol;
    bs->origPan = sfxPan;
    bs->curVolume = std::min((sfxVol * vol) >> 10, 127);
    bs->curPan = pan;
    bs->appVolume = vol;
    bs->appPan = pan;
    bs->appPb = bs->curPb = (params.mask & SNDPPM_PitchBend) ? params.pb : 0;
    bs->appPm = bs->curPm = (params.mask & SNDPPM_PitchMod) ? params.pm : 0;
    if (params.mask & SNDPPM_Registers) bs->registers = params.areg;
    bs->countdown = sfx.grains[0].delay;
    while (bs->countdown <= 0 && !bs->done) ExecuteEffectGrain(*bs);
    return bs;
}

// Voices that ended (a one-shot sample played out, or an envelope at its end).
void CleanEffectVoices(BLOCKSOUND& bs)
{
    for (EFFECTVOICE& v : bs.voices)
    {
        if (!v.voice) continue;
        XAUDIO2_VOICE_STATE state{};
        v.voice->GetState(&state);
        if ((state.BuffersQueued == 0 && v.noiseCore < 0) || v.envelopePhase == EffectEnvelope_Stopped) DestroyEffectVoice(v);
    }
    bs.voices.erase(std::remove_if(bs.voices.begin(), bs.voices.end(), [](const EFFECTVOICE& v) { return !v.voice; }),
                    bs.voices.end());
}

// The 240 Hz tick of one sound: its voices' envelopes (200 samples of 48 kHz), LFOs, children, AutoVol, grains.
// orig: (IOP) 989snd BlockSoundHandler::Tick; Sly1-Release UpdateEffectEnvelopes
bool FTickSound(BLOCKSOUND& bs)
{
    for (EFFECTVOICE& v : bs.voices)
    {
        if (!v.voice) continue;
        if (!bs.paused && !FGroupPaused(bs.group)) AdvanceEffectEnvelope(v, 200);
        ApplyVoiceVolume(bs, v);
        FeedNoiseVoice(v);
    }
    CleanEffectVoices(bs);
    TickEffectLfos(bs);
    for (auto it = bs.children.begin(); it != bs.children.end();)
    {
        if (FTickSound(**it)) it = bs.children.erase(it);
        else ++it;
    }
    if (bs.autoVolTicks > 0 && !bs.paused)
    {
        bs.autoVolPos += bs.autoVolStep;
        if (--bs.autoVolTicks == 0) bs.autoVolPos = bs.autoVolTarget << 8;
        SetVolPan(bs, bs.autoVolPos >> 8, SND_PAN_DONT_CHANGE);
        if (bs.autoVolTicks == 0 && bs.autoVolStop)
        {
            bs.done = true;
            for (EFFECTVOICE& v : bs.voices) ReleaseEffectEnvelope(v);
        }
    }
    if (bs.done && bs.children.empty()) return bs.voices.empty();
    if (bs.paused || FGroupPaused(bs.group)) return false;
    bs.countdown--;
    while (bs.countdown <= 0 && !bs.done) ExecuteEffectGrain(bs);
    return false;
}

// orig: (IOP) 989snd.irx 0xEDA0 (a sound stopped: by the EE, an instance limit, its STOP grain, its parent): its
// children stopped; the first time, a sound with an on-stop marker (grain 0x2B, not the last: 0x49CC) runs the grains
// after it at once (0x4870) and lives on while they play; else its LFOs stop (0x2440) and its voices are keyed off
// (0x1A438), the sound freed once they end
void StopBlockSound(BLOCKSOUND& bs)
{
    for (auto& child : bs.children) StopBlockSound(*child);
    if (!bs.stopping)
    {
        bs.stopping = true;
        const std::vector<GRAIN>& ag = bs.Sound().grains;
        int iMark = -1;
        for (int i = 0; i + 1 < static_cast<int>(ag.size()) && iMark < 0; ++i)
            if (ag[size_t(i)].type == GRAINK_OnStopMarker) iMark = i;
        if (iMark >= 0)
        {
            bs.done = false;
            bs.skipGrains = false;
            bs.nextGrain = iMark + 1;
            for (int c = 0; !bs.done && c < 1024; ++c) ExecuteEffectGrain(bs);
            if (!bs.voices.empty() || !bs.children.empty()) return;
        }
    }
    bs.done = true;
    bs.lfoStopped = true;
    bs.tickStop = s_tick;
    for (EFFECTVOICE& v : bs.voices) ReleaseEffectEnvelope(v);
}

void DestroyBlockSound(BLOCKSOUND& bs)
{
    for (auto& child : bs.children) DestroyBlockSound(*child);
    for (EFFECTVOICE& v : bs.voices) DestroyEffectVoice(v);
    bs.voices.clear();
    bs.children.clear();
}

void PauseBlockSound(BLOCKSOUND& bs, bool fPause)
{
    bs.paused = fPause;
    for (auto& child : bs.children) PauseBlockSound(*child, fPause);
    for (EFFECTVOICE& v : bs.voices)
    {
        if (!v.voice) continue;
        if (fPause) v.voice->Stop();
        else v.voice->Start();
        ApplyVoiceVolume(bs, v);
    }
}

void AutoVolBlockSound(BLOCKSOUND& bs, int vol, int ticks)
{
    const bool stop = vol == -4;
    const int target = stop ? 0 : std::clamp(vol, 0, 0x400);
    for (auto& child : bs.children) AutoVolBlockSound(*child, vol, ticks);
    if (ticks <= 0)
    {
        bs.autoVolTicks = 0;
        if (stop) StopBlockSound(bs);
        else SetVolPan(bs, target, SND_PAN_DONT_CHANGE);
        return;
    }
    bs.autoVolTarget = target;
    bs.autoVolStop = stop;
    bs.autoVolTicks = ticks;
    bs.autoVolPos = bs.appVolume << 8;
    bs.autoVolStep = ((target - bs.appVolume) << 8) / ticks;
}

BLOCKSOUND* PbsFind(uint32_t h)
{
    const auto it = s_mpsound.find(h);
    return it == s_mpsound.end() ? nullptr : it->second.get();
}

EFFECTBANK* PbankFind(uint32_t h)
{
    const auto it = s_mpbank.find(h);
    return it == s_mpbank.end() ? nullptr : it->second.get();
}

// 989snd's master volume duckers (4; orig: (IOP) 989snd.irx 0x1C224 snd_SetMasterVolumeDucker, export 119): while a
// voice of the source group plays, the target groups' duck factor falls to the full duck in attack steps, then
// rises back in release steps.
struct DUCKER
{
    int srcGroup = -1;            // +0x00 (-1 unused)
    uint32_t grfTarget = 0;       // +0x04 the target groups' mask
    int fullDuck = 0x10000;       // +0x08 16.16
    int stepAttack = 1;           // +0x0C (set from the attack time: (0x10000 - full) / (t * 30 >> 16))
    int stepRelease = 1;          // +0x10
    int cur = 0x10000;            // +0x14
};
std::array<DUCKER, 4> s_aducker{};

void UpdateAllVoiceVolumes()
{
    for (auto& [h, pbs] : s_mpsound)
        for (EFFECTVOICE& v : pbs->voices) ApplyVoiceVolume(*pbs, v);
    SpiopMasterChanged();
}

// orig: (IOP) 989snd.irx 0x1BFB4 (every 4th tick): the groups with a playing voice (not paused), each ducker
// stepped, each target group's factor the lowest of its duckers', the voices updated when one changed
void UpdateDuckers()
{
    uint32_t grfActive = SpiopActiveGroups();
    for (const auto& [h, pbs] : s_mpsound)
    {
        if (pbs->paused || FGroupPaused(pbs->group) || pbs->group < 0 || pbs->group >= 32) continue;
        for (const EFFECTVOICE& v : pbs->voices)
            if (v.voice && v.envelopePhase != EffectEnvelope_Stopped) grfActive |= 1u << pbs->group;
    }
    std::array<uint32_t, 15> agDuck{};
    agDuck.fill(0x10000);
    for (DUCKER& d : s_aducker)
    {
        if (d.srcGroup == -1) continue;
        if (grfActive & (1u << d.srcGroup))
        {
            if (d.cur != d.fullDuck)
            {
                d.cur -= d.stepAttack;
                if (d.cur < d.fullDuck) d.cur = d.fullDuck;
            }
        }
        else
        {
            if (d.cur == 0x10000) continue;
            d.cur += d.stepRelease;
            if (static_cast<uint32_t>(d.cur) > 0x10000u) d.cur = 0x10000;
        }
        for (int g = 0; g < 15; ++g)
            if ((d.grfTarget & (1u << g)) && static_cast<uint32_t>(d.cur) < agDuck[size_t(g)]) agDuck[size_t(g)] = static_cast<uint32_t>(d.cur);
    }
    if (agDuck != s_agroupduck)
    {
        s_agroupduck = agDuck;
        UpdateAllVoiceVolumes();
    }
}

void TickAll()
{
    ++s_tick;
    if ((s_tick & 3) == 0) UpdateDuckers();
    for (auto it = s_mpsound.begin(); it != s_mpsound.end();)
    {
        if (FTickSound(*it->second))
        {
            DestroyBlockSound(*it->second);
            it = s_mpsound.erase(it);
        }
        else
            ++it;
    }
    SpiopUpdate();
}

void ThreadMain()
{
    cane::sched::RegisterAudioThread();   // (Windows: MMCSS "Audio"; the 1 ms timer is the process's: platform/game_sched.h)
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / kTickHz));
    auto next = clock::now();
    while (s_fThread.load(std::memory_order_acquire))
    {
        {
            const auto tWake = clock::now();
            std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
            // (debug, SNDCOUNT: how late the ticks run: the wake after the due time plus the wait for the lock; a noise
            // voice queues 15 ms, a lag beyond that starves it)
            const double msLate = std::chrono::duration<double, std::milli>(clock::now() - next).count();
            const double msLock = std::chrono::duration<double, std::milli>(clock::now() - tWake).count();
            s_msTickLateMax = std::max(s_msTickLateMax, msLate);
            s_msTickLockMax = std::max(s_msTickLockMax, msLock);
            if (msLate > 12.0) ++s_cTickLate;
            // Ticks counted from elapsed time (a late wake runs the missed ones, at most 8).
            for (int i = 0; i < 8 && clock::now() >= next; ++i)
            {
                TickAll();
                next += period;
            }
            if (clock::now() >= next) next = clock::now() + period;
        }
        std::this_thread::sleep_until(next);
    }
}
}  // namespace

void snd_StartSoundSystem()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (s_fStarted) return;
    s_fStarted = true;
    FEnsureAudio();
    s_fThread = true;
    s_thread = std::thread(ThreadMain);
}

void snd_StopSoundSystem()
{
    if (s_fThread.exchange(false) && s_thread.joinable()) s_thread.join();
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    SpiopStopAll();
    for (auto& [h, pbs] : s_mpsound) DestroyBlockSound(*pbs);
    s_mpsound.clear();
    s_mpbank.clear();
    s_fStarted = false;
}

uint32_t snd_BankLoadByLoc(uint32_t isector, uint32_t ib)
{
    auto bank = std::make_unique<EFFECTBANK>();
    uint8_t head[24];
    if (!g_disc.FReadFileBytes(isector, ib, head, sizeof(head))) return 0;
    uint32_t type = 0, cchunk = 0;
    std::memcpy(&type, head, 4);
    std::memcpy(&cchunk, head + 4, 4);
    if ((type != 1 && type != 3) || cchunk < 2 || cchunk > 8) return 0;
    std::vector<uint8_t> attr(8 + 8 * cchunk);
    if (!g_disc.FReadFileBytes(isector, ib, attr.data(), attr.size())) return 0;
    uint32_t cb = 0;
    for (uint32_t i = 0; i < cchunk; ++i)
    {
        uint32_t off = 0, size = 0;
        std::memcpy(&off, &attr[8 + 8 * i], 4);
        std::memcpy(&size, &attr[12 + 8 * i], 4);
        cb = std::max(cb, off + size);
    }
    if (cb == 0 || cb > 64u << 20) return 0;
    bank->bytes.resize(cb);
    if (!g_disc.FReadFileBytes(isector, ib, bank->bytes.data(), cb)) return 0;
    if (!Parse989SfxBank(*bank))
    {
        std::fprintf(stderr, "snd: bad bank at sector 0x%X\n", isector);
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    bank->isector = isector;
    // (PC) the installed bank's samples are WAVs beside it; a bank from the mods folder plays its own samples
    bank->fWavSamples = ib == 0 && g_disc.FConverted() && !g_disc.FModded(isector);
    bank->hbank = 0x00010000u | (s_hbankNext++ & 0xFFu) << 8;
    const uint32_t h = bank->hbank;
    if (std::getenv("SLY2N_SNDLOG"))
        std::printf("snd: bank %08x loaded from sector 0x%x (%zu bytes, %zu sounds)\n", h, isector, bank->bytes.size(),
                    bank->sounds.size());
    s_mpbank[h] = std::move(bank);
    return h;
}

void snd_UnloadBank(uint32_t hbank)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    EFFECTBANK* pbank = PbankFind(hbank);
    if (!pbank) return;
    for (auto it = s_mpsound.begin(); it != s_mpsound.end();)
    {
        if (it->second->pbank == pbank)
        {
            DestroyBlockSound(*it->second);
            it = s_mpsound.erase(it);
        }
        else
            ++it;
    }
    s_mpbank.erase(hbank);
}

void snd_ResolveBankXREFS() {}

void snd_SetMasterVolume(int group, int vol)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (group < 0 || group >= static_cast<int>(s_amastervol.size())) return;
    s_amastervol[group] = vol;
    if (group == SND_GROUP_MASTER)
    {
        SetOutputVolume(std::clamp(vol, 0, 0x400) / 1024.0f);
        return;
    }
    for (auto& [h, pbs] : s_mpsound)
        for (EFFECTVOICE& v : pbs->voices) ApplyVoiceVolume(*pbs, v);
    SpiopMasterChanged();
}

// orig: (IOP) 989snd.irx 0x1C224 (export 119): ducker which (0..3) from {source group, target mask, full duck,
// attack, release} (16.16; times in seconds), or unused (nullptr)
void snd_SetMasterVolumeDucker(int which, const SNDDUCKER* pdef)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (which < 0 || which >= 4) return;
    DUCKER& d = s_aducker[size_t(which)];
    if (!pdef)
    {
        d.srcGroup = -1;
        return;
    }
    d.srcGroup = pdef->srcGroup;
    d.grfTarget = pdef->grfTarget;
    d.fullDuck = pdef->fullDuck;
    const int span = 0x10000 - d.fullDuck;
    auto step = [&](int t) {
        int n = (t * 30) >> 16;
        if (n == 0) n = 1;
        const int s = span / n;
        return s == 0 ? 1 : s;
    };
    d.stepAttack = step(pdef->tAttack);
    d.stepRelease = step(pdef->tRelease);
    d.cur = 0x10000;
}

int snd_GetMasterVolume(int group)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    return group >= 0 && group < static_cast<int>(s_amastervol.size()) ? s_amastervol[group] : 0;
}

int snd_IsSoundALooper(uint32_t hbank, int isound)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const EFFECTBANK* pbank = PbankFind(hbank);
    return pbank && isound >= 0 && isound < static_cast<int>(pbank->sounds.size()) && pbank->sounds[isound].IsLooper();
}

// orig: (IOP) 989snd snd_PlaySoundEx (Player::PlaySoundWithParams) with the instance limit check
uint32_t snd_PlaySoundEx(const SNDPLAYPARAMS& params)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (!(params.mask & SNDPPM_Bank) || !(params.mask & SNDPPM_Sound)) return 0;  // (by name: unused by Sly 2)
    EFFECTBANK* pbank = PbankFind(params.hbank);
    if (!pbank || params.isound < 0 || params.isound >= static_cast<int>(pbank->sounds.size())) return 0;
    SOUND& sfx = pbank->sounds[params.isound];
    const int vol = (params.mask & SNDPPM_Vol) ? params.vol : 0x400;
    if (static const char* s_sz = std::getenv("SLY2N_SNDPARAMS"); s_sz && (std::atoi(s_sz) == params.isound || std::atoi(s_sz) == -1))   // (debug: one sound's plays)
    {
        int c = 0;
        for (auto& [h, pbs] : s_mpsound) c += pbs->pbank == pbank && pbs->isound == params.isound;
        std::printf("sndparams: sound %d mask 0x%X vol %d pan %d pm %d pb %d (sound vol %d group %d limit %d flags 0x%X grains %zu) playing %d\n",
                    params.isound, params.mask, params.vol, params.pan, params.pm, params.pb, sfx.volume, sfx.volumeGroup,
                    sfx.instanceLimit, sfx.flags, sfx.grains.size(), c);
        static bool s_afDumped[1024];
        if (std::getenv("SLY2N_NOISEDBG") && params.isound < 1024 && !s_afDumped[params.isound])
        {
            s_afDumped[params.isound] = true;
            for (const GRAIN& g : sfx.grains)
                std::printf("  grain type %u delay %d param %d %d %d %d tone flags 0x%X center %d adsr %04X %04X lfo type %d target %d depth %d\n", g.type, g.delay,
                            g.param[0], g.param[1], g.param[2], g.param[3], g.tone.flags, g.tone.centerNote, g.tone.adsr1, g.tone.adsr2,
                            int(g.lfo.shape), int(g.lfo.target), int(g.lfo.depth));
        }
    }
    // CheckInstanceLimit: flags 8 limit, 0x10 the quietest goes, 0x20 the oldest goes.
    if (sfx.instanceLimit && (sfx.flags & 8))
    {
        BLOCKSOUND* weakest = nullptr;
        int inst = 0;
        for (auto& [h, pbs] : s_mpsound)
        {
            if (pbs->pbank != pbank || (pbs->isoundOrig != params.isound && pbs->isound != params.isound)) continue;
            inst++;
            if (!weakest || ((sfx.flags & 0x10) && pbs->appVolume < weakest->appVolume) ||
                ((sfx.flags & 0x20) && pbs->startTick < weakest->startTick))
                weakest = pbs.get();
        }
        if (sfx.instanceLimit - 1 < inst)
        {
            if (weakest && (((sfx.flags & 0x10) && weakest->appVolume < vol) || (sfx.flags & 0x20)))
                StopBlockSound(*weakest);
            else
                return 0;
        }
    }
    auto pbs = PbsNew(pbank, params.isound, -1, -1, params, s_tick);
    if (!pbs) return 0;
    const uint32_t h = 0x01000000u | (++s_hsoundSerial & 0xFFFFFFu);
    pbs->h = h;
    s_mpsound[h] = std::move(pbs);
    return h;
}

void snd_StopSound(uint32_t h)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (BLOCKSOUND* pbs = PbsFind(h)) StopBlockSound(*pbs);
}

void snd_PauseSound(uint32_t h)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (BLOCKSOUND* pbs = PbsFind(h)) PauseBlockSound(*pbs, true);
}

void snd_ContinueSound(uint32_t h)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (BLOCKSOUND* pbs = PbsFind(h)) PauseBlockSound(*pbs, false);
}

void snd_StopAllSounds()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    for (auto& [h, pbs] : s_mpsound) StopBlockSound(*pbs);
}

void snd_PauseAllSoundsInGroup(uint32_t mask)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    s_grouppausemask |= mask;
    for (auto& [h, pbs] : s_mpsound)
        if (FGroupPaused(pbs->group)) PauseBlockSound(*pbs, true);
    SpiopPauseGroups(mask, true);
}

void snd_ContinueAllSoundsInGroup(uint32_t mask)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    for (auto& [h, pbs] : s_mpsound)
        if (pbs->group >= 0 && pbs->group < 32 && (mask & (1u << pbs->group))) PauseBlockSound(*pbs, false);
    s_grouppausemask &= ~mask;
    SpiopPauseGroups(mask, false);
}

uint32_t snd_SoundIsStillPlaying(uint32_t h)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (FSpiopStreamHandle(h)) return h;
    return PbsFind(h) ? h : 0;
}

// orig: (IOP) 989snd snd_SetSoundParams (0xcc34): mask 1 vol, 2 / 4 pan, 8 pitch modifier, 0x10 pitch bend
void snd_SetSoundParams(uint32_t h, uint32_t mask, int vol, int pan, int pm, int pb)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if ((mask & 6) == 6) mask &= ~2u;
    BLOCKSOUND* pbs = PbsFind(h);
    if (!pbs) return;
    if (mask & (SNDSPM_Vol | SNDSPM_Pan | SNDSPM_PanB))
        SetVolPan(*pbs, (mask & SNDSPM_Vol) ? vol : SND_VOLUME_DONT_CHANGE,
                  (mask & (SNDSPM_Pan | SNDSPM_PanB)) ? pan : SND_PAN_DONT_CHANGE);
    if (mask & SNDSPM_PitchMod) SetPMod(*pbs, static_cast<int16_t>(pm));
    if (mask & SNDSPM_PitchBend) SetPBend(*pbs, static_cast<int16_t>(pb));
}

void snd_AutoVol(uint32_t h, int vol, int ticks, int from)
{
    (void)from;
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (BLOCKSOUND* pbs = PbsFind(h)) AutoVolBlockSound(*pbs, vol, ticks);
}

int snd_BankSoundCount(uint32_t hbank)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const EFFECTBANK* pbank = PbankFind(hbank);
    return pbank ? static_cast<int>(pbank->sounds.size()) : 0;
}

uint32_t snd_BankId(uint32_t hbank)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const EFFECTBANK* pbank = PbankFind(hbank);
    return pbank ? pbank->bankId : 0;
}

int snd_CSoundsActive()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    return static_cast<int>(s_mpsound.size());
}

int snd_CVoicesActive()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    int c = 0;
    std::vector<const BLOCKSOUND*> stack;
    for (auto& [h, pbs] : s_mpsound) stack.push_back(pbs.get());
    while (!stack.empty())
    {
        const BLOCKSOUND* p = stack.back();
        stack.pop_back();
        c += CLiveVoices(*p);
        for (auto& child : p->children) stack.push_back(child.get());
    }
    return c;
}

// orig: (IOP) 989snd.irx 0x10DE0 / 0x10E50: a VAG stream channel's voice (the stream's group, priority 0x7F, locked)
int snd_AllocStreamVoice(int group)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    (void)group;  // (every group's range is 0..47)
    return FVoiceLimit() ? AllocSpuVoice(0, kcSpuVoice - 1, 0x7F, true, SPUVS_Stream) : -1;
}

// orig: (IOP) 989snd.irx 0x1AEC0 (the stream's voices freed with it)
void snd_FreeStreamVoice(int iv)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (iv >= 0 && iv < kcSpuVoice && s_aspuvoice[size_t(iv)].status == SPUVS_Stream) FreeSpuVoice(iv);
}

SNDSPUSTATS snd_SpuStats()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    SNDSPUSTATS st;
    for (const SPUVOICE& sv : s_aspuvoice)
    {
        st.cUsed += sv.status != SPUVS_Free;
        st.cStream += sv.status == SPUVS_Stream;
    }
    st.cPeak = s_spustats.cPeak;
    st.cAlloc = s_spustats.cAlloc;
    st.cSteal = s_spustats.cSteal;
    st.cFail = s_spustats.cFail;
    // voices of a stopped sound still playing 4 s after the stop (a release is at most ~1 s at 989snd's rates)
    std::vector<const BLOCKSOUND*> stack;
    for (auto& [h, pbs] : s_mpsound) stack.push_back(pbs.get());
    while (!stack.empty())
    {
        const BLOCKSOUND* p = stack.back();
        stack.pop_back();
        st.cVoiceHost += CLiveVoices(*p);
        if (p->done && p->lfoStopped && s_tick > p->tickStop + 4 * kTickHz) st.cStale += CLiveVoices(*p);
        for (auto& c : p->children) stack.push_back(c.get());
    }
    return st;
}

std::vector<SNDENDLESS> snd_EndlessLoopers()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    std::vector<SNDENDLESS> a;
    const EFFECTBANK* pbankFirst = s_mpbank.empty() ? nullptr : s_mpbank.begin()->second.get();
    for (auto& [h, pbs] : s_mpsound)
    {
        if (pbs->done || pbs->stopping) continue;
        const SOUND& snd = pbs->pbank->sounds[size_t(pbs->isoundOrig)];
        if (!snd.IsLooper()) continue;
        if (std::any_of(snd.grains.begin(), snd.grains.end(), [](const GRAIN& g) { return g.type == GRAINK_Stop; })) continue;
        a.push_back({h, pbs->isoundOrig, pbs->pbank == pbankFirst, s_tick - pbs->startTick});
    }
    return a;
}

std::string snd_Describe()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    char ach[200];
    const SNDSPUSTATS st = snd_SpuStats();
    std::snprintf(ach, sizeof(ach), "%zu banks, %zu sounds, %d voices (spu %d/48: %d streams, peak %d, stolen %llu, "
                  "dropped %llu, stale %d), %s", s_mpbank.size(), s_mpsound.size(), snd_CVoicesActive(), st.cUsed, st.cStream,
                  st.cPeak, static_cast<unsigned long long>(st.cSteal), static_cast<unsigned long long>(st.cFail), st.cStale,
                  SpiopDescribe().c_str());
    std::string str = ach;
    // (the host's effect voices made and destroyed: made - destroyed beyond the voices above would be leaked FAudio voices)
    str += ", host voices made " + std::to_string(s_cHostVoiceCreated) + " destroyed " + std::to_string(s_cHostVoiceDestroyed);
    char achTick[96];
    std::snprintf(achTick, sizeof(achTick), ", ticks late max %.1f ms (lock %.1f), %llu over 12 ms", s_msTickLateMax, s_msTickLockMax,
                  static_cast<unsigned long long>(s_cTickLate));
    str += achTick;
    s_msTickLateMax = s_msTickLockMax = 0.0;
    if (!s_mpnoisedenied.empty())
    {
        str += ", noise denied";
        for (const auto& [i, c] : s_mpnoisedenied) str += " " + (i < 0 ? "k" + std::to_string(-1 - i) : std::to_string(i)) + "x" + std::to_string(c);
        s_mpnoisedenied.clear();
    }
    // (debug: SLY2N_SNDLIST: the live sounds by bank sound: count x voices)
    if (static const bool s_fList = std::getenv("SLY2N_SNDLIST") != nullptr; s_fList)
    {
        std::map<int, std::pair<int, int>> mp, mpKept;   // (kept: a bank other than the first loaded: pcloader's kept worlds)
        const EFFECTBANK* pbankFirst = s_mpbank.empty() ? nullptr : s_mpbank.begin()->second.get();
        for (auto& [h, pbs] : s_mpsound)
        {
            int cv = 0;
            std::vector<const BLOCKSOUND*> stack{pbs.get()};
            while (!stack.empty())
            {
                const BLOCKSOUND* p = stack.back();
                stack.pop_back();
                cv += CLiveVoices(*p);
                for (auto& c : p->children) stack.push_back(c.get());
            }
            auto& e = (pbs->pbank == pbankFirst ? mp : mpKept)[pbs->isoundOrig];
            ++e.first, e.second += cv;
        }
        str += "\n  sounds:";
        for (auto& [is, e] : mp) str += " " + std::to_string(is) + ":" + std::to_string(e.first) + "x" + std::to_string(e.second);
        if (!mpKept.empty())
        {
            str += "; other banks:";
            for (auto& [is, e] : mpKept) str += " " + std::to_string(is) + ":" + std::to_string(e.first) + "x" + std::to_string(e.second);
        }
        // (SLY2N_SNDINST=isound: that bank sound's instances: age in ticks, next grain, countdown, done / paused / stopping)
        if (static const char* s_szInst = std::getenv("SLY2N_SNDINST"); s_szInst)
            for (auto& [h, pbs] : s_mpsound)
                if (pbs->isoundOrig == std::atoi(s_szInst))
                    str += "\n  inst " + std::to_string(h) + ": age " + std::to_string(s_tick - pbs->startTick) + " grain " +
                           std::to_string(pbs->nextGrain) + "/" + std::to_string(pbs->Sound().grains.size()) + " countdown " +
                           std::to_string(pbs->countdown) + " done " + std::to_string(pbs->done) + " paused " + std::to_string(pbs->paused) +
                           " stopping " + std::to_string(pbs->stopping) + " group " + std::to_string(pbs->group) + " voices " +
                           std::to_string(CLiveVoices(*pbs));
        // (SLY2N_SNDLIST=2: the SPU voice table, each voice's owner (k: another bank) and priority)
        if (static const bool s_fVoices = std::atoi(std::getenv("SLY2N_SNDLIST")) >= 2; s_fVoices)
        {
            str += "\n  noise cores: " + std::to_string(s_anoisecore[0].priority) + (s_anoisecore[0].fLock ? "L" : "") + " " +
                   std::to_string(s_anoisecore[1].priority) + (s_anoisecore[1].fLock ? "L" : "") + "; spu:";
            for (const SPUVOICE& sv : s_aspuvoice)
            {
                if (sv.status == SPUVS_Free) { str += " -"; continue; }
                if (!sv.pbs) { str += " s" + std::to_string(sv.priority); continue; }
                str += " " + std::string(sv.pbs->pbank == pbankFirst ? "" : "k") + std::to_string(sv.pbs->isoundOrig) +
                       (sv.pbs->pbank->sounds[size_t(sv.pbs->isoundOrig)].IsLooper() ? "L" : "") + "@" + std::to_string(sv.priority);
            }
        }
    }
    return str;
}

void snd_StreamLevels(int vol, int pan, int chanPan, int group, float* pgLeft, float* pgRight)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const VOLPAIR p = MakeVolume(127, chanPan, vol, pan, 127, 0);
    *pgLeft = static_cast<float>(AdjustVolToGroup(p.left, group) >> 1) / 16383.0f;
    *pgRight = static_cast<float>(AdjustVolToGroup(p.right, group) >> 1) / 16383.0f;
    if (static const bool s_f = std::getenv("SLY2N_MOVIEAUDIT") != nullptr; s_f)   // (debug: the stream's group levels)
        std::printf("streamlevels: group %d master %d duck 0x%X master(16) %d -> %.3f %.3f\n", group,
                    group >= 0 && group < 32 ? s_amastervol[size_t(group)] : -1, group >= 0 && group < 15 ? s_agroupduck[size_t(group)] : 0u,
                    s_amastervol[SND_GROUP_MASTER], *pgLeft, *pgRight);
}

bool snd_FGroupPaused(int group)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    return FGroupPaused(group);
}

void snd_Tick()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    TickAll();
}

// (the converter) Every sample a bank's tones play, decoded as EffectToneSample does.
bool snd_DecodeBankSamples(const std::vector<uint8_t>& bytes, std::vector<SNDBANKSAMPLE>& asample)
{
    EFFECTBANK bank;
    bank.bytes = bytes;
    if (!Parse989SfxBank(bank)) return false;
    std::map<uint32_t, std::vector<int>> mpoffsetasound;
    for (std::size_t isound = 0; isound < bank.sounds.size(); ++isound)
        for (const GRAIN& g : bank.sounds[isound].grains)
        {
            if (g.type != GRAINK_Tone && g.type != GRAINK_Tone2) continue;
            if ((g.tone.flags & 8) || g.tone.sampleOffset >= bank.sampleSize) continue;  // (noise: no sample)
            auto& a = mpoffsetasound[g.tone.sampleOffset];
            if (a.empty() || a.back() != int(isound)) a.push_back(int(isound));
        }
    for (const auto& [offset, asound] : mpoffsetasound)
    {
        SNDBANKSAMPLE s;
        DecodedEffectSample d;
        DecodeSpuAdpcm(bank.bytes, std::size_t(bank.sampleOffset) + offset, std::size_t(bank.sampleOffset) + bank.sampleSize,
                       d.pcm, d.loop, d.loopBegin, d.loopLength);
        if (d.pcm.empty()) continue;
        s.offset = offset;
        s.pcm = std::move(d.pcm);
        s.loop = d.loop;
        s.loopBegin = d.loopBegin;
        s.loopLength = d.loopLength;
        s.asound = asound;
        asample.push_back(std::move(s));
    }
    return true;
}
