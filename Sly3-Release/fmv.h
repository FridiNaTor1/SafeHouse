#pragma once

// The game's movies: Sony PSS files on the disc (MPEG-2 program streams: MPEG-2 video in 0xE0, the sound in
// private stream 1 as an "SShd"/"SSbd" ADS stream of SPU2 ADPCM, stereo, interleaved by channel). The original
// plays them with libmpeg on the IPU (0x1B2820..0x1B4728) and feeds the sound to 989snd's movie voices; here the
// PSS is demuxed straight from the disc image while it plays (no extraction), the video decoded by FFmpeg
// (Sly1-Release fmv.cpp's decoder setup), the sound decoded into an XAudio2 stream (audio.h) that is the clock.
//
// Movie keys: the 64-byte file records in the executable (Sly 3: orig3 0x3179F8, 23 records, the size's low 14 bits
// masked; disc.h CDisc::Movies), those whose file starts with an MPEG pack header.

#include "disc.h"

#include <cstdint>
#include <string>
#include <vector>

struct MOVIEINFO
{
    uint32_t va = 0;  // the key's address in the executable
    FK fk;
};

// The movies on the disc, from the executable's keys.
std::vector<MOVIEINFO> MovieList();
// A movie by its file id (FK::id, e.g. 338), the key's address (0x3179F8...), or its index in MovieList.
bool FFindMovie(const std::string& arg, MOVIEINFO* pinfo);

// What shows the pictures: called with each RGBA picture (cx * cy * 4 bytes, top row first) when it is due;
// returns false to stop the movie (skipped / window closed). Also called with null between pictures (about every
// 4 ms) so the presenter can poll its input.
using PFNMOVIEPRESENT = bool (*)(const uint8_t* prgba, int cx, int cy, void* pv);

struct MOVIESTATS
{
    int cx = 0, cy = 0;
    double fps = 0.0;
    int cframe = 0;
    double tAudio = 0.0;  // seconds of sound decoded
    int audioRate = 0, audioChannels = 0, audioFormat = 0, audioInterleave = 0;
    bool fSkipped = false;
};

// Plays the movie to its end (or until the presenter says stop, or tMax seconds); false when it could not play.
bool FPlayMovie(const FK& fk, PFNMOVIEPRESENT pfnPresent, void* pv, MOVIESTATS* pstats = nullptr, double tMax = 1e9);

// --list-movies / --play-movie ID (an SDL2 window; with --out DIR also a picture every 2 s as PPM files)
int ListMoviesCommand();
int PlayMovieCommand(const std::string& arg, const std::string& dirOut, double tMax);
