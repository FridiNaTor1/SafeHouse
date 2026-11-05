// 989snd on the host (see snd989.h). Adapted from Sly1-Release/sound.cpp (EFFECTBANK, the grain executor,
// the SPU2 ADSR and LFOs, the tone voices on XAudio2); the 989snd rules from the reference port's 989snd
// (Sly2-Reference/Sly2-Release/third_party/989snd: OpenGOAL's reimplementation, which the reference checked against the IRX):
// v2/v3 grain layout, MakeVolume / AdjustVolToGroup, PS1Note2Pitch / PitchBend, instance limits, AutoVol.

#include "snd989.h"

#include "audio.h"
#include "disc.h"
#include "spiop.h"

// FAudio on every platform (on Windows too, over SDL2), so the mixer behaves the same everywhere.
#include "../platform/audio_linux.h"

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

std::shared_ptr<const DecodedEffectSample> EffectToneSample(const EFFECTBANK& bank, const TONE& tone)
{
    if (tone.flags & 8)  // SPU2 noise (Sly1-Release: one LFSR period, looped)
    {
        static const std::shared_ptr<const DecodedEffectSample> s_noise = [] {
            static constexpr uint8_t noiseAdd[64] = {1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1,
                                                     1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
                                                     1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1};
            DecodedEffectSample d;
            uint16_t noise = 1;
            d.pcm.resize(32767);
            for (int16_t& s : d.pcm)
            {
                noise = static_cast<uint16_t>((noise << 1) | noiseAdd[(noise >> 10) & 63]);
                s = static_cast<int16_t>(noise);
            }
            d.loop = true;
            d.loopLength = static_cast<uint32_t>(d.pcm.size());
            return std::make_shared<const DecodedEffectSample>(std::move(d));
        }();
        return s_noise;
    }
    const auto it = bank.decodedSamples.find(tone.sampleOffset);
    if (it != bank.decodedSamples.end()) return it->second;
    if (tone.sampleOffset >= bank.sampleSize) return nullptr;
    DecodedEffectSample d;
    DecodeSpuAdpcm(bank.bytes, std::size_t(bank.sampleOffset) + tone.sampleOffset,
                   std::size_t(bank.sampleOffset) + bank.sampleSize, d.pcm, d.loop, d.loopBegin, d.loopLength);
    if (d.pcm.empty()) return nullptr;
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

// orig: (IOP) 989snd MakeVolume (Sly2-Reference third_party vagvoice.cpp)
VOLPAIR MakeVolume(int vol1, int pan1, int vol2, int pan2, int vol3, int pan3)
{
    int vol = vol1 * 258;
    vol = (vol * vol2) / 0x7F;
    vol = (vol * vol3) / 0x7F;
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

// orig: (IOP) 989snd AdjustVolToGroup: the group's master, then squared (v3 squares the product)
int AdjustVolToGroup(int volume, int group)
{
    if (group >= 15) return volume;
    if (volume >= 0x7FFF) volume = 0x7FFE;
    volume = volume * s_amastervol[group] / 0x400;
    const int sign = volume < 0 ? -1 : 1;
    return volume * volume / 0x7FFE * sign;
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
    bool skipGrains = false;
    int grainsToPlay = 0, grainsToSkip = 0;
    uint64_t startTick = 0;
    int autoVolTicks = 0, autoVolTarget = 0, autoVolPos = 0, autoVolStep = 0;
    bool autoVolStop = false;
    std::vector<EFFECTVOICE> voices;
    std::vector<std::unique_ptr<BLOCKSOUND>> children;

    SOUND& Sound() const { return pbank->sounds[isound]; }
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

void DestroyEffectVoice(EFFECTVOICE& v)
{
    if (!v.voice) return;
    v.voice->Stop();
    v.voice->DestroyVoice();
    v.voice = nullptr;
    v.sample.reset();
    v.envelopePhase = EffectEnvelope_Stopped;
}

bool FGroupPaused(int group) { return group >= 0 && group < 32 && (s_grouppausemask & (1u << group)) != 0; }

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
    }
    SetVoiceLevels(v.voice, 1, gl, gr);
}

void ApplyVoicePitch(const BLOCKSOUND& bs, EFFECTVOICE& v)
{
    if (!v.voice) return;
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
void PlayEffectTone(BLOCKSOUND& bs, const TONE& tone)
{
    bs.curVolume = std::clamp(((bs.appVolume * bs.origVolume) >> 10) + bs.lfoVolume, 0, 127);
    int p = (bs.appPan + bs.lfoPan) % 360;
    bs.curPan = p < 0 ? p + 360 : p;
    if (!FEnsureAudio()) return;
    const std::shared_ptr<const DecodedEffectSample> decoded = EffectToneSample(*bs.pbank, tone);
    if (!decoded) return;

    EFFECTVOICE v;
    v.sample = decoded;
    v.tone = tone;
    v.loop = decoded->loop;
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

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    if (FAILED(g_pxaudio2->CreateSourceVoice(&v.voice, &format, 0, kSpuMaximumFrequencyRatio)) || !v.voice)
    {
        v.voice = nullptr;
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
        v.voice->DestroyVoice();
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
        if (grain.psp.soundId >= 0)
            bs.children.erase(std::remove_if(bs.children.begin(), bs.children.end(),
                                             [&](const std::unique_ptr<BLOCKSOUND>& c) {
                                                 return c->isound == grain.psp.soundId;
                                             }),
                              bs.children.end());
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
    case GRAINK_Stop: bs.done = true; break;
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
        if (state.BuffersQueued == 0 || v.envelopePhase == EffectEnvelope_Stopped) DestroyEffectVoice(v);
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

// orig: (IOP) 989snd BlockSoundHandler::Stop
void StopBlockSound(BLOCKSOUND& bs)
{
    bs.done = true;
    for (auto& child : bs.children) StopBlockSound(*child);
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

void TickAll()
{
    ++s_tick;
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
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / kTickHz));
    auto next = clock::now();
    while (s_fThread.load(std::memory_order_acquire))
    {
        {
            std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
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
    const uint64_t at = uint64_t(isector) * 2048 + ib;
    uint8_t head[24];
    if (!g_disc.FReadBytes(at, head, sizeof(head))) return 0;
    uint32_t type = 0, cchunk = 0;
    std::memcpy(&type, head, 4);
    std::memcpy(&cchunk, head + 4, 4);
    if ((type != 1 && type != 3) || cchunk < 2 || cchunk > 8) return 0;
    std::vector<uint8_t> attr(8 + 8 * cchunk);
    if (!g_disc.FReadBytes(at, attr.data(), attr.size())) return 0;
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
    if (!g_disc.FReadBytes(at, bank->bytes.data(), cb)) return 0;
    if (!Parse989SfxBank(*bank))
    {
        std::fprintf(stderr, "snd: bad bank at sector 0x%X\n", isector);
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    bank->isector = isector;
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
        c += static_cast<int>(p->voices.size());
        for (auto& child : p->children) stack.push_back(child.get());
    }
    return c;
}

std::string snd_Describe()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    char ach[96];
    std::snprintf(ach, sizeof(ach), "%zu banks, %zu sounds, %d voices, %s", s_mpbank.size(), s_mpsound.size(),
                  snd_CVoicesActive(), SpiopDescribe().c_str());
    return ach;
}

void snd_StreamLevels(int vol, int pan, int chanPan, int group, float* pgLeft, float* pgRight)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const VOLPAIR p = MakeVolume(127, chanPan, vol, pan, 127, 0);
    *pgLeft = static_cast<float>(AdjustVolToGroup(p.left, group) >> 1) / 16383.0f;
    *pgRight = static_cast<float>(AdjustVolToGroup(p.right, group) >> 1) / 16383.0f;
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
