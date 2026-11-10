// spiop on the host (see spiop.h). The plugin's behaviour is the reference's reading of spiop.irx
// (Sly2-Reference/Sly2-Release/runtime/snd/snd_spiop.cpp: the plugin table at 0x54e8, the track thread 0x1e48, the header
// parse 0x20c4, the interleaved disc reads 0x2794) and its 989snd VAG streams (snd_stream.cpp: a stereo pair's
// voices at pans 270 / 90, the volume MakeVolume -> AdjustVolToGroup -> >> 1, the pitch modifier). Instead of
// SPU2 ring buffers the ADPCM is decoded straight from the disc into XAudio2 voices.

#include "spiop.h"

#include "audio.h"
#include "disc.h"
#include "snd989.h"
#include "flacfile.h"
#include "wavfile.h"

#include "../platform/platform.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace
{
constexpr uint32_t kSector = 2048;
constexpr int kTracks = 3;
constexpr uint32_t kChunk = 0x1000;  // a VPK's interleave per channel; also what one refill decodes
constexpr uint32_t kChunkFrames = kChunk / 16 * 28;  // (a WAV: the same frames per refill, at the same boundaries)

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t Le32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

struct TRACKFILE
{
    uint32_t lsn = 0;
    uint32_t nch = 1;
    uint32_t data = 0;       // byte offset of the data (0x30 for a VAGp, the header's for a VPK)
    uint32_t size = 0;       // data bytes per channel
    uint32_t loopStart = 0;  // per channel
    uint32_t rate = 0;
    bool vag = false;
    // A WAV (the converted installation's, or a mod's; wavfile.h): size, loopStart and sizeLoop are frames
    bool wav = false;
    WAVINFO wi;
    uint32_t sizeLoop = 0;   // where a looping track's data ends (the smpl loop's end)
    std::shared_ptr<const std::vector<int16_t>> pmem;  // a FLAC's frames, decoded whole at the start (flacfile.h)
};

struct CHANNEL
{
    int id = 0;
    int vol = 0x400;
    int pan = 0;
    int h1 = 0, h2 = 0;  // the ADPCM history
};

struct STREAM
{
    int ich0 = 0;
    int ich1 = -1;  // the right channel of a stereo pair
    int vol = 127;  // 989snd's 0..127
    int pan = 0;
    int pm = 0;
    std::unique_ptr<CPcmStream> pcm;
};

struct TRACK
{
    int state = -1;  // -1 free, 3 playing, 4 the data's end pushed (draining)
    uint32_t gen = 0;  // (native) counts its starts and frees: a refill read made meanwhile (SpiopUpdate) is not used
    uint32_t id = 0;
    int vol = 0, pan = 0;
    uint32_t flags = 0;
    bool headset = false;
    bool paused = false;  // fn 3 or SPIOPF_Paused
    bool groupPaused = false;
    TRACKFILE file;
    std::vector<CHANNEL> ch;
    std::vector<STREAM> streams;
    uint32_t pos = 0;  // per channel byte position of the next refill
    std::vector<int16_t> aPcm;
    std::vector<int> aspu;  // its channels' SPU2 voices in 989snd's table (one a channel, 0x10DE0)
};

TRACK s_atrack[kTracks];
uint32_t s_idCounter = 0;

// The channel ids of a file at lsn whose own table is not known: the installed file's (CDisc::PathInstalled), when it
// has cch channels; else every channel the installed file's single id (a dialog line: 0), else 5 (a plain stem).
std::vector<int> AidFallback(uint32_t lsn, int cch)
{
    std::vector<int> aid;
    const std::string path = g_disc.PathInstalled(lsn);
    if (!path.empty())
    {
        const PFNWAVREAD pfn = PfnReadFile(path);
        uint8_t h[0x40] = {};
        if (pfn(0, h, sizeof(h)))
        {
            std::error_code ec;
            WAVINFO wi;
            if (FIsWav(h, sizeof(h)) && FParseWav(pfn, std::filesystem::file_size(cane::path(path), ec), wi))
            {
                for (const WAVSTEM& st : wi.astem) aid.push_back(st.id);
                if (wi.astem.empty() && wi.cch == 1) aid.push_back(0);  // (a dialog line)
            }
            else if (!std::memcmp(h, "VAGp", 4) || FIsFlac(h, sizeof(h))) aid.push_back(0);
            else if (!std::memcmp(h, " KPV", 4))
                for (uint32_t i = 0; i < Le32(h + 0x14) && i < 6; ++i) aid.push_back(static_cast<int8_t>(h[0x18 + i * 4 + 1]));
        }
    }
    if (int(aid.size()) == cch) return aid;
    return std::vector<int>(std::size_t(cch), aid.size() == 1 ? aid[0] : 5);
}

// The track's file header (0x20c4): false when the sector holds neither a VAGp nor a VPK (nor a WAV or a FLAC).
bool FOpenFile(TRACK& t, uint32_t lsn)
{
    uint8_t h[kSector];
    if (!g_disc.FReadFileBytes(lsn, 0, h, sizeof(h))) return false;
    TRACKFILE f;
    f.lsn = lsn;
    t.ch.clear();
    if (FIsFlac(h, sizeof(h)))
    {
        // (PC) a FLAC (the European dialog lines): decoded whole, then played as a WAV without a smpl chunk
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<uint8_t> ab(std::size_t(g_disc.CbFile(lsn, 0)));
        auto pmem = std::make_shared<std::vector<int16_t>>();
        int rate = 0, cch = 0;
        std::string error;
        if (!g_disc.FReadFileBytes(lsn, 0, ab.data(), ab.size()) || !FDecodeFlac(ab, rate, cch, *pmem, &error) ||
            pmem->size() / std::size_t(cch) >= 0xFFFFFFFFu)
        {
            std::fprintf(stderr, "spiop: %s: %s\n", g_disc.PathOf(lsn).c_str(), error.c_str());
            return false;
        }
        if (std::getenv("SLY2N_SNDLOG"))
            std::printf("spiop: %s: FLAC decoded in %.1f ms\n", g_disc.PathOf(lsn).c_str(),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        f.wav = true;
        f.wi.rate = rate;
        f.wi.cch = cch;
        f.wi.cframe = pmem->size() / std::size_t(cch);
        f.pmem = std::move(pmem);
        f.nch = uint32_t(cch);
        f.rate = uint32_t(rate);
        f.size = f.sizeLoop = uint32_t(f.wi.cframe);
        f.loopStart = 0;
        for (int id : AidFallback(lsn, cch))
        {
            CHANNEL c;
            c.id = id;
            t.ch.push_back(c);
        }
    }
    else if (FIsWav(h, sizeof(h)))
    {
        // (PC) a WAV: the same frames the ADPCM decodes to, with the VPK's loop and channel table (wavfile.h)
        const PFNWAVREAD pfn = [lsn](uint64_t ib, void* pv, std::size_t cb) { return g_disc.FReadFileBytes(lsn, ib, pv, cb); };
        std::string error;
        if (!FParseWav(pfn, g_disc.CbFile(lsn, 0), f.wi, &error) || f.wi.cframe >= 0xFFFFFFFFull)
        {
            std::fprintf(stderr, "spiop: %s: %s\n", g_disc.PathOf(lsn).c_str(), error.c_str());
            return false;
        }
        f.wav = true;
        f.nch = uint32_t(f.wi.cch);
        f.rate = uint32_t(f.wi.rate);
        f.size = uint32_t(f.wi.cframe);
        f.sizeLoop = uint32_t(f.wi.CframeLooped());
        f.loopStart = uint32_t(f.wi.IframeLoopStart());
        std::vector<int> aid;
        for (const WAVSTEM& st : f.wi.astem) aid.push_back(st.id);
        if (aid.empty()) aid = AidFallback(lsn, f.wi.cch);
        for (int id : aid)
        {
            CHANNEL c;
            c.id = id;
            t.ch.push_back(c);
        }
    }
    else if (std::memcmp(h, "VAGp", 4) == 0)
    {
        f.vag = true;
        f.nch = 1;
        f.data = 0x30;
        f.size = Be32(h + 0xC);
        f.rate = Be32(h + 0x10);
        t.ch.push_back(CHANNEL{});
    }
    else if (std::memcmp(h, " KPV", 4) == 0)
    {
        const uint32_t nch = Le32(h + 0x14);
        if (Le32(h + 0xC) != 0x2000 || nch >= 6 || nch == 0) return false;
        f.nch = nch;
        f.size = Le32(h + 4);
        f.data = Le32(h + 8);
        f.rate = Le32(h + 0x10);
        f.loopStart = Le32(h + 0x7FC);
        for (uint32_t i = 0; i < nch; ++i)
        {
            CHANNEL c;
            c.id = static_cast<int8_t>(h[0x18 + i * 4 + 1]);
            t.ch.push_back(c);
        }
    }
    else
        return false;
    if (f.rate == 0) return false;
    t.file = f;
    return true;
}

// orig: (IOP) 989snd's volume of a set command (0x18394): 0..0x400 to 0..127, negative the absolute volume
int StreamVolOf(int v)
{
    if (v < 0) return std::min(-v, 0x7F);
    return std::min((v * 128 - v) >> 10, 0x7F);
}

int Wrap360(int p)
{
    p %= 360;
    return p < 0 ? p + 360 : p;
}

void ApplyStream(const TRACK& t, int itrack, STREAM& s)
{
    if (!s.pcm) return;
    float m[4] = {0, 0, 0, 0};
    if (!t.paused && !t.groupPaused)
    {
        if (s.ich1 < 0)
        {
            snd_StreamLevels(s.vol, s.pan, 0, itrack, &m[0], &m[2]);
        }
        else
        {
            snd_StreamLevels(s.vol, s.pan, 270, itrack, &m[0], &m[2]);  // the left channel's voice
            snd_StreamLevels(s.vol, s.pan, 90, itrack, &m[1], &m[3]);   // the right channel's
        }
    }
    for (float& g : m) g *= kStreamGain;
    // (debug: SLY2N_STREAMMUTE=mask silences tracks (1 ambience, 2 music, 4 dialog); SLY2N_STREAMLOG prints the levels)
    static const int s_grfMute = std::getenv("SLY2N_STREAMMUTE") ? std::atoi(std::getenv("SLY2N_STREAMMUTE")) : 0;
    if (s_grfMute & (1 << itrack))
        for (float& g : m) g = 0.0f;
    if (static const bool s_fLog = std::getenv("SLY2N_STREAMLOG") != nullptr; s_fLog)
        std::printf("spiop: track %d id %u ch %d vol %d pan %d -> %.4f %.4f %.4f %.4f\n", itrack, t.id, s.ich0, s.vol, s.pan,
                    m[0], m[1], m[2], m[3]);
    s.pcm->SetMatrix(m);
    s.pcm->SetFrequencyRatio(static_cast<float>(std::exp2(s.pm / 1536.0)));
}

void ApplyTrack(TRACK& t, int itrack)
{
    for (STREAM& s : t.streams) ApplyStream(t, itrack, s);
}

// A refill: the next stretch (to the next 0x1000 boundary of the channels' data) decoded into every stream; at the
// end a looping track goes back to its loop start (the ADPCM history carries on, as the SPU2's does), else the
// streams get their end. orig: (IOP) spiop 0x2794 (the interleaved read) + the SPU2 decoding. (PC) A WAV's: the same
// stretches in frames (to the next 7168-frame boundary, which is the ADPCM's 0x1000 bytes), so every stream is pushed
// the same frames in the same pieces as from the VPK / VAG it was converted from.
// In three parts, so the disc read needs no track state (SpiopUpdate can make it without the sound lock): the read
// prepared (the lock held), made, then decoded and pushed (the lock held, the track as it was prepared).
struct REFILL
{
    uint32_t gen = 0, pos = 0;   // the track's start and position it was prepared for
    bool wav = false;
    uint32_t lsn = 0;
    // a VPK / VAG: the chunk read
    uint64_t ib = 0;
    uint32_t cbRead = 0, chunkStart = 0, cbChunk = 0;
    std::vector<uint8_t> ab;
    // a WAV: the frames read (a FLAC's from its decoded frames)
    WAVINFO wi;
    std::shared_ptr<const std::vector<int16_t>> pmem;
    uint32_t nch = 1, chunkEnd = 0, cframe = 0;
    std::vector<int16_t> aframe;
};

// The loop's restart, or the data's end (the streams' end pushed, state 4: false, nothing to read); else the read.
bool FPrepareRefill(TRACK& t, REFILL& r)
{
    const TRACKFILE& f = t.file;
    const bool fLoop = (t.flags & SPIOPF_Loop) != 0;
    const uint32_t end = f.wav && fLoop ? f.sizeLoop : f.size;
    if (t.pos >= end)
    {
        if (fLoop && f.loopStart < end) t.pos = f.loopStart;
        else
        {
            for (STREAM& s : t.streams) s.pcm->FPush(nullptr, 0, true);
            t.state = 4;
            return false;
        }
    }
    r.gen = t.gen;
    r.pos = t.pos;
    r.wav = f.wav;
    r.lsn = f.lsn;
    if (f.wav)
    {
        r.chunkStart = t.pos / kChunkFrames * kChunkFrames;
        r.chunkEnd = std::min(r.chunkStart + kChunkFrames, end);
        r.cframe = r.chunkEnd - t.pos;
        r.nch = f.nch;
        r.wi = f.wi;
        r.pmem = f.pmem;
        return true;
    }
    r.chunkStart = t.pos & ~(kChunk - 1);
    r.cbChunk = std::min(kChunk, f.size - r.chunkStart);
    r.cbRead = f.vag ? r.cbChunk : kChunk * f.nch;  // (a VPK's last chunk is padded to the interleave)
    r.ib = f.data + uint64_t(r.chunkStart) * f.nch;
    return true;
}

// The read (no track state: safe without the sound lock).
bool FReadRefill(REFILL& r)
{
    if (!r.wav)
    {
        r.ab.assign(r.cbRead, 0);
        return g_disc.FReadFileBytes(r.lsn, r.ib, r.ab.data(), r.cbRead);
    }
    r.aframe.assign(std::size_t(r.cframe) * r.nch, 0);
    if (!r.cframe) return true;
    if (r.pmem)
    {
        std::copy_n(r.pmem->data() + std::size_t(r.pos) * r.nch, std::size_t(r.cframe) * r.nch, r.aframe.data());
        return true;
    }
    const PFNWAVREAD pfn = [lsn = r.lsn](uint64_t ib, void* pv, std::size_t cb) { return g_disc.FReadFileBytes(lsn, ib, pv, cb); };
    return FReadWavFrames(pfn, r.wi, r.pos, r.cframe, r.aframe.data());
}

// The data decoded and pushed into every stream (a failed read: the streams' end, state 4).
void ApplyRefill(TRACK& t, const REFILL& r, bool fRead)
{
    if (!fRead)
    {
        for (STREAM& s : t.streams) s.pcm->FPush(nullptr, 0, true);
        t.state = 4;
        return;
    }
    if (r.wav)
    {
        t.pos = r.chunkEnd;
        if (r.cframe == 0) return;
        for (STREAM& s : t.streams)
        {
            const int cch = s.ich1 < 0 ? 1 : 2;
            t.aPcm.assign(std::size_t(r.cframe) * cch, 0);
            for (int k = 0; k < cch; ++k)
            {
                const int ich = k == 0 ? s.ich0 : s.ich1;
                for (uint32_t i = 0; i < r.cframe; ++i) t.aPcm[std::size_t(i) * cch + k] = r.aframe[std::size_t(i) * r.nch + ich];
            }
            s.pcm->FPush(t.aPcm.data(), r.cframe);
        }
        return;
    }
    const uint32_t ibFrom = r.pos - r.chunkStart;
    const uint32_t cblock = (r.cbChunk - ibFrom) / 16;
    t.pos = r.chunkStart + r.cbChunk;
    if (cblock == 0) return;
    const bool fVag = t.file.vag;
    std::vector<int16_t> mono(cblock * 28);
    for (STREAM& s : t.streams)
    {
        const int cch = s.ich1 < 0 ? 1 : 2;
        t.aPcm.assign(std::size_t(cblock) * 28 * cch, 0);
        for (int k = 0; k < cch; ++k)
        {
            CHANNEL& c = t.ch[k == 0 ? s.ich0 : s.ich1];
            const uint8_t* pb = r.ab.data() + (fVag ? 0 : kChunk * (k == 0 ? s.ich0 : s.ich1)) + ibFrom;
            for (uint32_t b = 0; b < cblock; ++b) DecodeAdpcmBlock(pb + b * 16, &mono[b * 28], &c.h1, &c.h2);
            for (std::size_t i = 0; i < mono.size(); ++i) t.aPcm[i * cch + k] = mono[i];
        }
        s.pcm->FPush(t.aPcm.data(), mono.size());
    }
}

// A whole refill (the sound lock held).
void Refill(TRACK& t)
{
    REFILL r;
    if (FPrepareRefill(t, r)) ApplyRefill(t, r, FReadRefill(r));
}

void FreeTrack(TRACK& t)
{
    for (int iv : t.aspu) snd_FreeStreamVoice(iv);
    t.aspu.clear();
    for (STREAM& s : t.streams)
        if (s.pcm) s.pcm->Close();
    t.streams.clear();
    t.ch.clear();
    t.state = -1;
    t.id = 0;
    ++t.gen;
}

// States 1 and 2 (0x2410, 0x2560): the channels as streams (two channels with the same id: one stereo stream),
// buffered, started unless paused.
bool FStartStreams(TRACK& t, int itrack)
{
    const int n = static_cast<int>(t.ch.size());
    for (int i = 0; i < n; ++i) t.aspu.push_back(snd_AllocStreamVoice(itrack));
    for (int i = 0; i < n; ++i)
    {
        STREAM s;
        s.ich0 = i;
        if (i + 1 < n && t.ch[i + 1].id == t.ch[i].id) s.ich1 = ++i;
        s.vol = StreamVolOf(t.vol);
        s.pan = t.pan < 0 ? 0 : t.pan;
        s.pcm = std::make_unique<CPcmStream>();
        if (!s.pcm->FOpen(s.ich1 < 0 ? 1 : 2, static_cast<int>(t.file.rate))) return false;
        t.streams.push_back(std::move(s));
    }
    t.pos = 0;
    for (int i = 0; i < CPcmStream::kcBuffer - 1 && t.state == 3; ++i) Refill(t);
    t.groupPaused = snd_FGroupPaused(itrack);
    ApplyTrack(t, itrack);
    if (!t.paused && !t.groupPaused)
        for (STREAM& s : t.streams) s.pcm->Start();
    return true;
}

TRACK* PtrackFind(uint32_t id, int* pitrack = nullptr)
{
    for (int i = 0; i < kTracks; ++i)
        if (s_atrack[i].id == id && s_atrack[i].state != -1)
        {
            if (pitrack) *pitrack = i;
            return &s_atrack[i];
        }
    return nullptr;
}

void SetRunning(TRACK& t, int itrack)
{
    const bool run = !t.paused && !t.groupPaused;
    for (STREAM& s : t.streams)
        if (s.pcm) (run ? s.pcm->Start() : s.pcm->Stop());
    ApplyTrack(t, itrack);
}
}  // namespace

// orig: (IOP) spiop 0x400 (its start, 'SUKR' 0): 989snd's VAG streaming (10 streams), the voice ranges of groups
// 0..2 (0..3, 4..8, 9; not modelled) and ducker 0: while a group 2 (dialog) voice plays, every other group's master
// falls to 0.75 (0.5625 once squared) over 7 updates (989snd ducks at 60 Hz: 0.12 s) and comes back over 30 (0.5 s)
void SpiopInit()
{
    const SNDDUCKER d{2, ~4u, 0xC000, 0x4000, 0x10000};
    snd_SetMasterVolumeDucker(0, &d);
}

// The groups (tracks) with a stream playing, for 989snd's duckers (its voices are the stream voices of the track's
// group, not paused).
uint32_t SpiopActiveGroups()
{
    uint32_t grf = 0;
    for (int i = 0; i < kTracks; ++i)
    {
        const TRACK& t = s_atrack[i];
        if ((t.state == 3 || t.state == 4) && !t.paused && !t.groupPaused) grf |= 1u << i;
    }
    return grf;
}

// orig: (IOP) spiop fn 1 (0x7f4 / 0x1738); the EE side is 0x1FC510
uint32_t SpiopStart(uint32_t isector, int vol, int pan, int itrack, uint32_t flags, bool fHeadset)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (itrack < 0 || itrack >= kTracks) return 0;
    TRACK& t = s_atrack[itrack];
    if (t.state != -1) return 0;
    t.vol = vol;
    t.pan = pan;
    t.flags = flags;
    t.headset = fHeadset;
    t.paused = (flags & SPIOPF_Paused) != 0;
    t.id = ++s_idCounter;
    t.state = 3;
    ++t.gen;
    if (!FOpenFile(t, isector))
    {
        std::fprintf(stderr, "spiop: no VAGp / VPK at sector %u\n", isector);
        const uint32_t id = t.id;
        FreeTrack(t);
        return id;  // (the IRX's track stays opening; the id is valid, the track never plays)
    }
    if (std::getenv("SLY2N_SNDLOG"))
        std::printf("spiop: track %d id %u: sector %u, %u ch, %u Hz, vol %d pan %d flags %x\n", itrack, t.id, isector,
                    t.file.nch, t.file.rate, vol, pan, flags);
    if (!FStartStreams(t, itrack))
    {
        FreeTrack(t);
        return 0;
    }
    return t.id;
}

void SpiopStop(uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (TRACK* t = PtrackFind(id)) FreeTrack(*t);
}

void SpiopPause(uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    int i = 0;
    if (TRACK* t = PtrackFind(id, &i))
    {
        t->paused = true;
        SetRunning(*t, i);
    }
}

void SpiopContinue(uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    int i = 0;
    if (TRACK* t = PtrackFind(id, &i))
    {
        t->paused = false;
        SetRunning(*t, i);
    }
}

// orig: (IOP) spiop fn 5 (0x19a4)
void SpiopSetParams(uint32_t id, uint32_t mask, int vol, int pan, int pm)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    int i = 0;
    TRACK* t = PtrackFind(id, &i);
    if (!t) return;
    if (mask & 1) t->vol = vol;
    if (mask & 4) t->pan = pan;
    for (STREAM& s : t->streams)
    {
        const CHANNEL& c = t->ch[s.ich0];
        if (mask & 1) s.vol = StreamVolOf(c.vol * t->vol / 1024);
        if (mask & 6) s.pan = Wrap360(c.pan + t->pan);
        if (mask & 8) s.pm = static_cast<int16_t>(pm);
    }
    ApplyTrack(*t, i);
}

// orig: (IOP) spiop fn 6 (0x1b38)
void SpiopSetChannel(uint32_t id, int chanId, int vol, int pan)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    int i = 0;
    TRACK* t = PtrackFind(id, &i);
    if (!t || t->headset) return;
    for (CHANNEL& c : t->ch)
        if (c.id == chanId)
        {
            c.vol = vol;
            c.pan = pan;
        }
    for (STREAM& s : t->streams)
    {
        const CHANNEL& c = t->ch[s.ich0];
        if (c.id != chanId) continue;
        s.vol = StreamVolOf(c.vol * t->vol / 1024);
        s.pan = Wrap360(c.pan + t->pan);
        ApplyStream(*t, i, s);
    }
}

int SpiopFStarted(uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const TRACK* t = PtrackFind(id);
    return (!t || t->state >= 3 || t->state < 0) ? 1 : 0;
}

int SpiopFActive(uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    return PtrackFind(id) != nullptr;
}

// (debug: SLY2N_MUSICLAT) the seconds of a track the device has consumed (its first stream's frames played / rate), -1 none
double SpiopTConsumed(uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const TRACK* pt = PtrackFind(id);
    if (!pt || pt->streams.empty() || !pt->streams[0].pcm || !pt->streams[0].pcm->m_rate) return -1.0;
    return static_cast<double>(pt->streams[0].pcm->CFramePlayed()) / pt->streams[0].pcm->m_rate;
}

int CSpiopLiveTracks()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    int c = 0;
    for (const TRACK& t : s_atrack) c += t.state != -1;
    return c;
}

void SpiopStopAll()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    for (TRACK& t : s_atrack) FreeTrack(t);
}

// orig: (IOP) spiop's track thread, state 3 (refills) and 4 (0x26f0: the track freed once its streams ended).
// Each refill's disc read is made with the sound lock let go (the game thread waits on that lock): the track is
// checked again after the read, which is not used if the track stopped, restarted or moved meanwhile. Called with the
// lock already held (TickAll, except on the Switch), the read keeps it.
void SpiopUpdate()
{
    static REFILL s_r;   // (the sound thread's: its buffers kept)
    for (TRACK& t : s_atrack)
    {
        for (int guard = 0; guard < CPcmStream::kcBuffer; ++guard)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
                if (t.state == 4)
                {
                    bool fDone = true;
                    for (const STREAM& s : t.streams) fDone = fDone && s.pcm->CBufferQueued() == 0;
                    if (fDone) FreeTrack(t);
                    break;
                }
                if (t.state != 3) break;
                bool fRoom = true;
                for (const STREAM& s : t.streams) fRoom = fRoom && s.pcm->CBufferQueued() < CPcmStream::kcBuffer - 1;
                if (!fRoom || !FPrepareRefill(t, s_r)) break;
            }
            const bool fRead = FReadRefill(s_r);
            std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
            if (t.state != 3 || t.gen != s_r.gen || t.pos != s_r.pos) break;
            ApplyRefill(t, s_r, fRead);
            if (t.state != 3) break;   // (a failed read: the end pushed, freed at a later call once drained)
        }
    }
}

void SpiopMasterChanged()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    for (int i = 0; i < kTracks; ++i)
        if (s_atrack[i].state != -1) ApplyTrack(s_atrack[i], i);
}

void SpiopPauseGroups(uint32_t mask, bool fPause)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    for (int i = 0; i < kTracks; ++i)
    {
        TRACK& t = s_atrack[i];
        if (t.state == -1 || !(mask & (1u << i))) continue;
        t.groupPaused = fPause;
        SetRunning(t, i);
    }
}

bool FSpiopStreamHandle(uint32_t h)
{
    (void)h;
    return false;  // (the streams are spiop's tracks here, not 989snd handles the EE sees)
}

double SpiopTrackSeconds(uint32_t id)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const TRACK* t = PtrackFind(id);
    if (!t || t->streams.empty() || !t->streams[0].pcm) return 0.0;
    return static_cast<double>(t->streams[0].pcm->CFramePlayed()) / t->file.rate;
}

std::string SpiopDescribe()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    std::string out = "tracks:";
    char ach[96];
    for (int i = 0; i < kTracks; ++i)
    {
        const TRACK& t = s_atrack[i];
        if (t.state == -1) continue;
        std::snprintf(ach, sizeof(ach), " %d:id%u:lsn%u:%uch:%uHz:%.1fs%s", i, t.id, t.file.lsn, t.file.nch, t.file.rate,
                      t.streams.empty() ? 0.0 : double(t.streams[0].pcm->CFramePlayed()) / t.file.rate,
                      t.paused ? ":paused" : "");
        out += ach;
    }
    return out;
}

std::string SpiopDescribeFile(uint32_t isector)
{
    TRACK t;
    if (!FOpenFile(t, isector)) return "?";
    char ach[64];
    std::snprintf(ach, sizeof(ach), "%s %u Hz %.1f s ids", t.file.wav ? "WAV" : t.file.vag ? "VAGp" : "VPK", t.file.rate,
                  t.file.wav ? double(t.file.size) / t.file.rate : t.file.size / 16.0 * 28.0 / t.file.rate);
    std::string out = ach;
    for (const CHANNEL& c : t.ch) out += " " + std::to_string(c.id);
    if (!t.file.vag) out += " loop " + std::to_string(t.file.loopStart);
    return out;
}
