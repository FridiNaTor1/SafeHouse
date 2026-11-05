#pragma once

// FLAC files: the European installation's dialog lines and movie sound tracks
// (11 languages: as WAV they would make the installation 11.6 GB). Lossless: the same frames as the WAV would hold.
// Encoded and decoded with FFmpeg's libavcodec (already linked for the movies' video). No loop or stem metadata: a
// FLAC stream loops whole, like a WAV without a smpl chunk.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// True when the bytes start a FLAC file ("fLaC").
bool FIsFlac(const uint8_t* pb, std::size_t cb);
// A whole FLAC file decoded: interleaved 16-bit frames (24 / 32-bit streams rounded to 16).
bool FDecodeFlac(const std::vector<uint8_t>& ab, int& rate, int& cch, std::vector<int16_t>& asample, std::string* perror = nullptr);
bool FLoadFlac(const std::string& path, int& rate, int& cch, std::vector<int16_t>& asample, std::string* perror = nullptr);
// cch channels of 16-bit frames encoded (compression level 5): the file's bytes, empty on failure.
std::vector<uint8_t> AbFlac(int rate, int cch, const int16_t* asample, std::size_t cframe, std::string* perror = nullptr);
