#pragma once

// WAV files: the installation's audio and the mods'. Read through a callback (the
// installed files are read through CDisc, which applies the mods folder), written whole.
//
// What the game reads besides the samples:
// - `smpl` (the standard sampler chunk): the loop. Its first loop's start is where a looping stream or effect goes on
//   after its end (the end is the loop's end + 1, normally the file's end). A `smpl` with no loop: the sound never loops.
//   No `smpl`: a looping stream loops the whole file (as a VPK whose loop start is 0, and every VAG).
// - `stem` (SafeHouse's, music and ambience): each channel's stem {vol u8, id s8, pan s16 LE} after {u32 version 1,
//   u32 count}, the VPK channel table the music engine mixes by id (0 sneak, 1 normal, 2 alert, 3 / 4 the characters,
//   5 plain); two channels in a row with the same id are one stereo stem. Without it a file takes the replaced file's
//   table when the channel counts agree (spiop.cpp).
// Samples: 16-bit PCM written; 16 / 24 / 32-bit PCM and 32-bit float read (as 16-bit, rounded), any channel count and
// rate. More than two channels are written as WAVE_FORMAT_EXTENSIBLE (no speaker positions: the channels are stems).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct WAVSTEM
{
    uint8_t vol = 127;
    int8_t id = 5;
    int16_t pan = 0;
};

struct WAVINFO
{
    int rate = 0;
    int cch = 0;
    int bits = 16;
    int format = 1;              // 1 PCM, 3 float
    uint64_t ibData = 0;         // the data's offset in the file
    uint64_t cframe = 0;         // frames (one sample per channel)
    bool fSmpl = false;          // a `smpl` chunk
    bool fLoop = false;          // ... with a loop
    uint64_t iframeLoop = 0;     // the loop's first frame
    uint64_t iframeLoopEnd = 0;  // the frame after the loop's last
    std::vector<WAVSTEM> astem;  // the `stem` chunk (empty: none)

    // Where a looping stream goes on after its end (VPK semantics): the loop's start; no `smpl`: 0; a `smpl` without a
    // loop: cframe (never).
    [[nodiscard]] uint64_t IframeLoopStart() const { return !fSmpl ? 0 : fLoop ? iframeLoop : cframe; }
    // A looping stream's end: the loop's end (normally the file's end).
    [[nodiscard]] uint64_t CframeLooped() const { return fLoop && iframeLoopEnd > iframeLoop && iframeLoopEnd <= cframe ? iframeLoopEnd : cframe; }
};

using PFNWAVREAD = std::function<bool(uint64_t ib, void* pv, std::size_t cb)>;

// True when the bytes start a RIFF WAVE file.
bool FIsWav(const uint8_t* pb, std::size_t cb);
// Parses the chunks of a file of cbFile bytes.
bool FParseWav(const PFNWAVREAD& pfnRead, uint64_t cbFile, WAVINFO& info, std::string* perror = nullptr);
// cframe frames from iframe as 16-bit interleaved samples (info.cch per frame); frames past the end read as 0.
bool FReadWavFrames(const PFNWAVREAD& pfnRead, const WAVINFO& info, uint64_t iframe, std::size_t cframe, int16_t* asample);
// A whole file from disk (all frames, interleaved).
bool FLoadWav(const std::string& path, WAVINFO& info, std::vector<int16_t>& asample, std::string* perror = nullptr);
// A PFNWAVREAD over a file on disk (opened per read).
PFNWAVREAD PfnReadFile(const std::string& path);

// A 16-bit PCM WAV: cch channels interleaved; loop: fSmpl / fLoop / iframeLoop / iframeLoopEnd of the info are written
// (fSmpl false: no smpl chunk); astem: a stem chunk when not empty.
std::vector<uint8_t> AbWav(int rate, int cch, const int16_t* asample, std::size_t cframe, const WAVINFO* ploop = nullptr,
                           const std::vector<WAVSTEM>* pastem = nullptr);
bool FWriteWavFile(const std::string& path, const std::vector<uint8_t>& ab);
