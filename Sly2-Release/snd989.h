#pragma once

// 989snd, Sony's IOP sound library (Sly 2 links version 3, "banks and streams only": no MIDI), on the host.
// The game (EE) side talks to it through the wrappers at 0x2319E8..0x233130 (command numbers in
// Sly2-Reference/Sly2-Release/runtime/snd/NOTES.md); here they are plain calls. The engine is Sly1-Release's sound.cpp
// reimplementation (SFX banks, SPU2 ADPCM, ADSR, LFOs, grains on XAudio2/FAudio voices), with the 989snd v3
// rules Sly 2's banks need: SBlk version 3 (v2 layout: 8-byte grains, tones in the grain data), squared
// volumes through the group masters, PS1Note2Pitch.
//
// Volumes are 989snd's: application volume 0..0x400 (or -1..-127 absolute), pan degrees 0..359 (270 left,
// 90 right), pitch modifier in 1/128 semitones, pitch bend -0x8000..0x7FFF. Groups 0..15 have a master volume
// (0x400 unity); group 16 is the SPU2's master.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// SndPlayParams (the 0x30-byte block of snd_PlaySoundEx): the mask says which fields are set.
enum SNDPPM : uint32_t
{
    SNDPPM_Bank = 0x01,
    SNDPPM_Sound = 0x02,
    SNDPPM_Vol = 0x04,
    SNDPPM_Pan = 0x08,
    SNDPPM_PitchMod = 0x10,
    SNDPPM_PitchBend = 0x20,
    SNDPPM_Registers = 0x40,
};

struct SNDPLAYPARAMS
{
    uint32_t mask = SNDPPM_Bank | SNDPPM_Sound;  // +0x28
    uint32_t hbank = 0;                           // +0x00
    int isound = 0;                               // +0x10 (s16)
    int vol = 0x400;                              // +0x08
    int pan = 0;                                  // +0x0C
    int pm = 0;                                   // +0x20 (s16)
    int pb = 0;                                   // +0x22 (s16)
    std::array<int8_t, 4> areg{};                 // +0x24
};

// snd_SetSoundParams' mask
enum SNDSPM : uint32_t
{
    SNDSPM_Vol = 0x01,
    SNDSPM_Pan = 0x02,  // (2 and 4 both mean pan; 6 together is pan once)
    SNDSPM_PanB = 0x04,
    SNDSPM_PitchMod = 0x08,
    SNDSPM_PitchBend = 0x10,
};

constexpr int SND_VOLUME_DONT_CHANGE = 0x7FFFFFFF;
constexpr int SND_PAN_DONT_CHANGE = -2;
constexpr int SND_PAN_RESET = -1;
constexpr int SND_GROUP_MASTER = 16;

void snd_StartSoundSystem();                                      // orig: 0x2319E8 (989snd cmd 0)
void snd_StopSoundSystem();
uint32_t snd_BankLoadByLoc(uint32_t isector, uint32_t ib = 0);    // orig: 0x231FB0 (loader fno 3); 0: failed
void snd_UnloadBank(uint32_t hbank);                              // orig: 0x2321B8 (cmd 6)
void snd_ResolveBankXREFS();                                      // orig: 0x232188 (cmd 8)
void snd_SetMasterVolume(int group, int vol);                     // orig: 0x2321E8 (cmd 9)
// A master volume ducker (989snd export 119; spiop sets ducker 0 at its start: the dialog ducks the rest).
struct SNDDUCKER
{
    int srcGroup;        // the group whose playing voices duck
    uint32_t grfTarget;  // the ducked groups' mask
    int fullDuck;        // 16.16 (squared like the master, in AdjustVolToGroup)
    int tAttack;         // 16.16 seconds (steps every 4th tick: (0x10000 - full) / (t * 30 >> 16) each)
    int tRelease;
};
void snd_SetMasterVolumeDucker(int which, const SNDDUCKER* pdef);
int snd_GetMasterVolume(int group);                               // orig: 0x232220 (cmd 0xA)
int snd_IsSoundALooper(uint32_t hbank, int isound);               // orig: 0x2322A0 (cmd 0x1A)
uint32_t snd_PlaySoundEx(const SNDPLAYPARAMS& params);            // orig: 0x2322F0 / 0x232320 (cmd 0x5C)
void snd_StopSound(uint32_t h);                                   // orig: 0x232350 (cmd 0x15)
void snd_PauseSound(uint32_t h);                                  // orig: 0x2323A8 (cmd 0x13)
void snd_ContinueSound(uint32_t h);                               // orig: 0x2323D8 (cmd 0x14)
void snd_StopAllSounds();                                         // orig: 0x232408 (cmd 0x18)
void snd_PauseAllSoundsInGroup(uint32_t mask);                    // orig: 0x232438 (cmd 0x16)
void snd_ContinueAllSoundsInGroup(uint32_t mask);                 // orig: 0x232468 (cmd 0x17)
uint32_t snd_SoundIsStillPlaying(uint32_t h);                     // orig: 0x232498 (cmd 0x19): h, or 0
void snd_SetSoundParams(uint32_t h, uint32_t mask, int vol, int pan, int pm, int pb);  // orig: 0x232500 (cmd 0x21)
void snd_AutoVol(uint32_t h, int vol, int ticks, int from);       // orig: 0x232568 (cmd 0x22)

// Host extras.
int snd_BankSoundCount(uint32_t hbank);
uint32_t snd_BankId(uint32_t hbank);  // the SBlk's bank id
int snd_CSoundsActive();
int snd_CVoicesActive();
std::string snd_Describe();
// The SPU2 voice table (989snd's 48 voices): used now (stream channels among them), the peak, the allocations, the
// steals and the tones dropped for want of a voice; host voices (effect tones playing) and stale ones (a stopped
// sound's voices still playing 4 s later: never expected)
struct SNDSPUSTATS
{
    int cUsed = 0, cStream = 0, cPeak = 0, cVoiceHost = 0, cStale = 0;
    unsigned long long cAlloc = 0, cSteal = 0, cFail = 0;
};
SNDSPUSTATS snd_SpuStats();
// (debug: the playing instances of looping sounds without a STOP grain (they end only by a stop): handle, bank sound,
// whether its bank is the first loaded one (the world's), age in 240 Hz ticks)
struct SNDENDLESS { uint32_t h = 0; int isound = -1; bool fFirstBank = true; uint64_t cTick = 0; };
std::vector<SNDENDLESS> snd_EndlessLoopers();
int snd_AllocStreamVoice(int group);  // a stream channel's voice (-1: none / the limit off)
void snd_FreeStreamVoice(int iv);
// A VAG stream voice's levels (989snd: MakeVolume(127, chanPan, vol 0..127, pan, 127, 0), the group's master,
// >> 1), 1.0 = the SPU2's 0x3FFF.
void snd_StreamLevels(int vol, int pan, int chanPan, int group, float* pgLeft, float* pgRight);
bool snd_FGroupPaused(int group);
// Runs one 240 Hz tick by hand (the sound thread does it normally; for tools without the thread).
void snd_Tick();

// (the converter) Every sample a bank's tones play (noise tones have none), decoded as the engine plays it (the SPU2
// ADPCM to the loop-end block, the loop at its converged history), with the sounds that use it (in sound order).
struct SNDBANKSAMPLE
{
    uint32_t offset = 0;       // in the bank's sample chunk (the tones' sample offset)
    std::vector<int16_t> pcm;  // mono, 48 kHz at the tone's center note
    bool loop = false;
    uint32_t loopBegin = 0, loopLength = 0;
    std::vector<int> asound;
};
bool snd_DecodeBankSamples(const std::vector<uint8_t>& bytes, std::vector<SNDBANKSAMPLE>& asample);
