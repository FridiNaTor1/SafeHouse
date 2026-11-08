#pragma once

// spiop: Sucker Punch's IOP plugin to 989snd ('SUKR', 0x524B5553) that streams the music and the dialog from
// the disc. Three tracks (index 0 the world's resident ambience, 1 the music, 2 dialog; the index is also the
// 989snd volume group). A track plays a "VAGp" file (dialog: mono, its rate in the header) or a " KPV" VPK
// (music: 2..5 channels interleaved by 0x1000 bytes, header 0x800, {vol, id, pan} per channel at 0x18, loop
// start at 0x7FC; two channels with the same id are one stereo stream). The game's sound object sets each
// channel id's volume (the music's stems). (PC) A converted installation's tracks are WAV (FLAC for the European
// dialog): the frames the ADPCM decodes to, the VPK's channel table and loop in the WAV's chunks (wavfile.h).
//
// The EE calls them through 'SUKR' plugin messages (0x233048 / 0x233088 / 0x2330D0 / 0x233130), function
// numbers below. Here the streams are decoded from the disc on the sound thread into XAudio2 voices.

#include <cstdint>
#include <string>

enum SPIOPF : uint32_t
{
    SPIOPF_Paused = 0x1,  // start paused (the game continues it once every track is buffered)
    SPIOPF_Loop = 0x4,
};

// fn 1: a track from a file at isector; vol 0..0x400, pan degrees; its id (counter from 1), 0 when the index is
// out of range or the track busy.
uint32_t SpiopStart(uint32_t isector, int vol, int pan, int itrack, uint32_t flags, bool fHeadset = false);
void SpiopStop(uint32_t id);                          // fn 2
void SpiopPause(uint32_t id);                         // fn 3
void SpiopContinue(uint32_t id);                      // fn 4
void SpiopSetParams(uint32_t id, uint32_t mask, int vol, int pan, int pm);  // fn 5: mask 1 vol, 4 pan, 8 pitch
void SpiopSetChannel(uint32_t id, int chanId, int vol, int pan);           // fn 6: one channel id's vol / pan
int SpiopFStarted(uint32_t id);                       // fn 7: 1 once playing (or done)
int SpiopFActive(uint32_t id);                        // fn 8
void SpiopStopAll();
int CSpiopLiveTracks();  // (debug: tracks not free)                                  // fn 10

// Host side.
void SpiopInit();  // 'SUKR' 0 at boot: the dialog ducker
uint32_t SpiopActiveGroups();
void SpiopUpdate();  // the sound thread's refill (240 Hz)
void SpiopMasterChanged();
void SpiopPauseGroups(uint32_t mask, bool fPause);
bool FSpiopStreamHandle(uint32_t h);
double SpiopTrackSeconds(uint32_t id);  // the track's play time
std::string SpiopDescribe();
// The channel ids of a file (VAGp: one channel id 0), for tools.
std::string SpiopDescribeFile(uint32_t isector);
double SpiopTConsumed(uint32_t id);   // (debug: SLY2N_MUSICLAT) a track's seconds the device consumed, -1 none
