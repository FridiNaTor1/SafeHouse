// spiop on the host (see spiop.h). The plugin's behaviour is the reference's reading of spiop.irx
// (Sly2-Reference/Sly2-Release/runtime/snd/snd_spiop.cpp: the plugin table at 0x54e8, the track thread 0x1e48, the header
// parse 0x20c4, the interleaved disc reads 0x2794) and its 989snd VAG streams (snd_stream.cpp: a stereo pair's
// voices at pans 270 / 90, the volume MakeVolume -> AdjustVolToGroup -> >> 1, the pitch modifier). Instead of
// SPU2 ring buffers the ADPCM is decoded straight from the disc into XAudio2 voices.

#include "spiop.h"

#include "audio.h"
#include "disc.h"
#include "snd989.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
constexpr uint32_t kSector = 2048;
constexpr int kTracks = 3;
constexpr uint32_t kChunk = 0x1000;  // a VPK's interleave per channel; also what one refill decodes

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
    std::vector<uint8_t> abChunk;
    std::vector<int16_t> aPcm;
};

TRACK s_atrack[kTracks];
uint32_t s_idCounter = 0;

// The track's file header (0x20c4): false when the sector holds neither a VAGp nor a VPK.
bool FOpenFile(TRACK& t, uint32_t lsn)
{
    uint8_t h[kSector];
    if (!g_disc.FReadBytes(uint64_t(lsn) * kSector, h, sizeof(h))) return false;
    TRACKFILE f;
    f.lsn = lsn;
    t.ch.clear();
    if (std::memcmp(h, "VAGp", 4) == 0)
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
    s.pcm->SetMatrix(m);
    s.pcm->SetFrequencyRatio(static_cast<float>(std::exp2(s.pm / 1536.0)));
}

void ApplyTrack(TRACK& t, int itrack)
{
    for (STREAM& s : t.streams) ApplyStream(t, itrack, s);
}

// Decodes the next stretch (to the next 0x1000 boundary of the channels' data) into every stream; at the end
// a looping track goes back to its loop start (the ADPCM history carries on, as the SPU2's does), else the
// streams get their end. orig: (IOP) spiop 0x2794 (the interleaved read) + the SPU2 decoding
bool FRefill(TRACK& t)
{
    const TRACKFILE& f = t.file;
    if (t.pos >= f.size)
    {
        if ((t.flags & SPIOPF_Loop) && f.loopStart < f.size) t.pos = f.loopStart;
        else
        {
            for (STREAM& s : t.streams) s.pcm->FPush(nullptr, 0, true);
            t.state = 4;
            return false;
        }
    }
    const uint32_t chunkStart = t.pos & ~(kChunk - 1);
    const uint32_t cbChunk = std::min(kChunk, f.size - chunkStart);
    const uint32_t cbRead = f.vag ? cbChunk : kChunk * f.nch;  // (a VPK's last chunk is padded to the interleave)
    const uint64_t at = uint64_t(f.lsn) * kSector + f.data + uint64_t(chunkStart) * f.nch;
    t.abChunk.assign(cbRead, 0);
    if (!g_disc.FReadBytes(at, t.abChunk.data(), cbRead))
    {
        for (STREAM& s : t.streams) s.pcm->FPush(nullptr, 0, true);
        t.state = 4;
        return false;
    }
    const uint32_t ibFrom = t.pos - chunkStart;
    const uint32_t cblock = (cbChunk - ibFrom) / 16;
    t.pos = chunkStart + cbChunk;
    if (cblock == 0) return true;
    std::vector<int16_t> mono(cblock * 28);
    for (STREAM& s : t.streams)
    {
        const int cch = s.ich1 < 0 ? 1 : 2;
        t.aPcm.assign(std::size_t(cblock) * 28 * cch, 0);
        for (int k = 0; k < cch; ++k)
        {
            CHANNEL& c = t.ch[k == 0 ? s.ich0 : s.ich1];
            const uint8_t* pb = t.abChunk.data() + (f.vag ? 0 : kChunk * (k == 0 ? s.ich0 : s.ich1)) + ibFrom;
            for (uint32_t b = 0; b < cblock; ++b) DecodeAdpcmBlock(pb + b * 16, &mono[b * 28], &c.h1, &c.h2);
            for (std::size_t i = 0; i < mono.size(); ++i) t.aPcm[i * cch + k] = mono[i];
        }
        s.pcm->FPush(t.aPcm.data(), mono.size());
    }
    return true;
}

void FreeTrack(TRACK& t)
{
    for (STREAM& s : t.streams)
        if (s.pcm) s.pcm->Close();
    t.streams.clear();
    t.ch.clear();
    t.state = -1;
    t.id = 0;
}

// States 1 and 2 (0x2410, 0x2560): the channels as streams (two channels with the same id: one stereo stream),
// buffered, started unless paused.
bool FStartStreams(TRACK& t, int itrack)
{
    const int n = static_cast<int>(t.ch.size());
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
    for (int i = 0; i < CPcmStream::kcBuffer - 1 && t.state == 3; ++i) FRefill(t);
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

// orig: (IOP) spiop's track thread, state 3 (refills) and 4 (0x26f0: the track freed once its streams ended)
void SpiopUpdate()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    for (TRACK& t : s_atrack)
    {
        if (t.state == 3)
        {
            for (int guard = 0; guard < CPcmStream::kcBuffer && t.state == 3; ++guard)
            {
                bool fRoom = true;
                for (const STREAM& s : t.streams) fRoom = fRoom && s.pcm->CBufferQueued() < CPcmStream::kcBuffer - 1;
                if (!fRoom) break;
                FRefill(t);
            }
        }
        else if (t.state == 4)
        {
            bool fDone = true;
            for (const STREAM& s : t.streams) fDone = fDone && s.pcm->CBufferQueued() == 0;
            if (fDone) FreeTrack(t);
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
    std::snprintf(ach, sizeof(ach), "%s %u Hz %.1f s ids", t.file.vag ? "VAGp" : "VPK", t.file.rate,
                  t.file.size / 16.0 * 28.0 / t.file.rate);
    std::string out = ach;
    for (const CHANNEL& c : t.ch) out += " " + std::to_string(c.id);
    if (!t.file.vag) out += " loop " + std::to_string(t.file.loopStart);
    return out;
}
