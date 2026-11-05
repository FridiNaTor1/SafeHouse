#pragma once

// Music tools (MODDING.md, "Sly 1 / Music"). Sly 1's music files (Worlds/Music/MUSID_<id>.mus) are 989snd banks:
// ADPCM instrument samples played by a MIDI sequence. These tools need no game data:
//
//   --make-mus OUT.mus IN.wav [--loop SECONDS | --loop none] [--volume 0..127]
//       A bank holding the WAV as one looping sample (left and right), played by a one-note sequence.
//   --export-wav IN.mus OUT.wav [--seconds N]
//       Plays the bank through the game's own music engine for N seconds (default 120), captured to a WAV.
//       (Linux / Steam Deck: SDL's disk audio driver; it takes N seconds.)

#include <string>
#include <vector>

// Runs a tool when args[0] names one; returns its exit code, or -1 when args[0] is not a music tool.
int RunMusicTool(const std::vector<std::string>& args);
