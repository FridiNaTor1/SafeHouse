// Movies (see fmv.h). The PSS demux is Sly1-Release's "File Extractor/main.cpp" DemuxPss / DecodePssAudioToWav
// made incremental (packets read from the disc as the movie plays); the decoder setup is Sly1-Release fmv.cpp's.

#include "fmv.h"
#include "region.h"

#include "audio.h"
#include "snd989.h"
#include "flacfile.h"
#include "wavfile.h"

#include "../platform/platform.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace
{
constexpr uint32_t kSector = 2048;

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t Be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
uint32_t Le32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

// The PSS file read from the disc in pieces.
class CPssReader
{
public:
    explicit CPssReader(const FK& fk) : m_isector(fk.isector), m_cbFile(std::size_t(g_disc.CbFile(fk.isector, fk.cb)))
    {
        // (PC) a converted installation's movie is its MPEG-2 video stream alone (.m2v, from a sequence header), the
        // sound a WAV beside it: the stream is read in packet-sized pieces as video
        uint8_t h[4] = {};
        m_fElementary = g_disc.FReadFileBytes(m_isector, 0, h, 4) && Be32(h) == 0x000001B3;
    }
    [[nodiscard]] bool FElementary() const { return m_fElementary; }

    // The next PES packet of the video (0xE0) or private stream 1 (0xBD); false at the end.
    bool FNextPacket(uint32_t* pid, const uint8_t** ppb, std::size_t* pcb)
    {
        if (m_fElementary)
        {
            const std::size_t cb = std::min<std::size_t>(kSector, m_cbFile - std::min(m_ib, m_cbFile));
            if (cb == 0 || !FEnsure(cb)) return false;
            *pid = 0xE0;
            *ppb = Pb();
            *pcb = cb;
            m_ib += cb;
            return true;
        }
        for (;;)
        {
            if (!FEnsure(4)) return false;
            const uint8_t* p = Pb();
            const uint32_t code = Be32(p);
            if (code == 0x000001B9) return false;  // program end
            if (code == 0x000001BA)
            {
                if (!FEnsure(14)) return false;
                m_ib += 14 + (Pb()[13] & 7u);
                continue;
            }
            if (code == 0x000001BB || code == 0x000001BE || ((code >> 8) == 1 && code != 0x000001E0 && code != 0x000001BD &&
                                                             (code & 0xFF) >= 0xBC))
            {
                if (!FEnsure(6)) return false;
                m_ib += 6 + Be16(Pb() + 4);
                continue;
            }
            if (code != 0x000001E0 && code != 0x000001BD)
            {
                ++m_ib;  // zero fill: resynchronize on the next start code
                continue;
            }
            if (!FEnsure(9)) return false;
            const std::size_t cbPacket = Be16(Pb() + 4);
            if (!FEnsure(6 + cbPacket)) return false;
            p = Pb();
            const std::size_t cbExt = p[8];
            if (cbPacket < 3 + cbExt)
            {
                m_ib += 6 + cbPacket;
                continue;
            }
            *pid = code & 0xFF;
            *ppb = p + 9 + cbExt;
            *pcb = cbPacket - 3 - cbExt;
            m_ib += 6 + cbPacket;
            return true;
        }
    }

private:
    const uint8_t* Pb() const { return m_ab.data() + (m_ib - m_ibBuf); }

    bool FEnsure(std::size_t cb)
    {
        if (m_ib + cb <= m_ibBuf + m_ab.size()) return true;
        if (m_ib + cb > m_cbFile) return false;
        // Keep the unread tail, read the next 64 KB after it.
        std::vector<uint8_t> ab(m_ab.begin() + static_cast<std::ptrdiff_t>(m_ib - m_ibBuf), m_ab.end());
        m_ibBuf = m_ib;
        const std::size_t cbWant = std::max<std::size_t>(cb, 0x10000);
        const std::size_t cbHave = ab.size();
        const std::size_t cbRead = std::min<std::size_t>(cbWant, m_cbFile - (m_ibBuf + cbHave));
        ab.resize(cbHave + cbRead);
        if (cbRead && !g_disc.FReadFileBytes(m_isector, m_ibBuf + cbHave, ab.data() + cbHave, cbRead)) return false;
        m_ab = std::move(ab);
        return m_ib + cb <= m_ibBuf + m_ab.size();
    }

    uint32_t m_isector;  // (the file: its key's sector)
    std::size_t m_cbFile;
    bool m_fElementary = false;
    std::vector<uint8_t> m_ab;
    std::size_t m_ibBuf = 0;  // file offset of m_ab[0]
    std::size_t m_ib = 0;     // file offset of the parse
};

// The movie's sound: the ADS stream (SShd header, SSbd body) joined from the private stream's packets (each with
// a 4-byte tag first), decoded per interleave unit into stereo PCM.
// orig: (EE) the movie sound path (CQueueOutputIop, 989snd type 3 voices); Sly1-Release DecodePssAudioToWav
class CMovieAudio
{
public:
    void AddPacket(const uint8_t* pb, std::size_t cb)
    {
        if (cb <= 4) return;
        m_ab.insert(m_ab.end(), pb + 4, pb + cb);
        if (!m_fHeader) ParseHeader();
        if (m_fHeader) Decode();
    }

    // (PC) the sound from a WAV (a converted installation's movie, a mod's): the frames as the ADS decodes them
    bool FOpenWav(const std::string& path)
    {
        uint8_t h[4] = {};
        if (PfnReadFile(path)(0, h, 4) && FIsFlac(h, 4))
        {
            // a FLAC (the European discs' tracks): decoded whole, read as a WAV in memory
            int rate = 0, cch = 0;
            std::string error;
            auto pmem = std::make_shared<std::vector<int16_t>>();
            if (!FLoadFlac(path, rate, cch, *pmem, &error))
            {
                std::fprintf(stderr, "movie: %s: %s\n", path.c_str(), error.c_str());
                return false;
            }
            m_wi = WAVINFO{};
            m_wi.rate = rate;
            m_wi.cch = cch;
            m_wi.bits = 16;
            m_wi.cframe = pmem->size() / std::size_t(cch);
            m_pfnWav = [pmem](uint64_t ib, void* pv, std::size_t cb) {
                if (ib + cb > pmem->size() * 2) return false;
                std::memcpy(pv, reinterpret_cast<const uint8_t*>(pmem->data()) + ib, cb);
                return true;
            };
            m_fWav = true;
            m_fHeader = true;
            m_format = 1;
            m_rate = uint32_t(rate);
            m_cch = uint32_t(std::min(cch, 2));
            return true;
        }
        m_pfnWav = PfnReadFile(path);
        std::error_code ec;
        const uint64_t cb = std::filesystem::file_size(cane::path(path), ec);
        std::string error;
        if (ec || !FParseWav(m_pfnWav, cb, m_wi, &error))
        {
            std::fprintf(stderr, "movie: %s: %s\n", path.c_str(), ec ? "cannot read" : error.c_str());
            m_pfnWav = nullptr;
            return false;
        }
        m_fWav = true;
        m_fHeader = true;
        m_format = 1;
        m_rate = uint32_t(m_wi.rate);
        m_cch = uint32_t(std::min(m_wi.cch, 2));
        return true;
    }
    // Reads the WAV on until a second of sound waits (in 8192-frame pieces, the pieces queueAudio pushes).
    void TopUpWav()
    {
        if (!m_fWav) return;
        std::vector<int16_t> a;
        while (m_iframeWav < m_wi.cframe && m_pcm.size() < m_rate * 2u)
        {
            const std::size_t c = std::size_t(std::min<uint64_t>(8192, m_wi.cframe - m_iframeWav));
            a.resize(c * std::size_t(m_wi.cch));
            if (!FReadWavFrames(m_pfnWav, m_wi, m_iframeWav, c, a.data())) { m_iframeWav = m_wi.cframe; break; }
            for (std::size_t i = 0; i < c; ++i)
            {
                m_pcm.push_back(a[i * std::size_t(m_wi.cch)]);
                m_pcm.push_back(a[i * std::size_t(m_wi.cch) + (m_wi.cch > 1 ? 1 : 0)]);
            }
            m_iframeWav += c;
            m_cframeDecoded += c;
        }
    }
    // All the sound is decoded (a WAV read to its end; a PSS's is when the file is).
    [[nodiscard]] bool FWavDone() const { return !m_fWav || m_iframeWav >= m_wi.cframe; }

    bool m_fHeader = false;
    uint32_t m_format = 0, m_rate = 0, m_cch = 0, m_interleave = 0;
    std::deque<int16_t> m_pcm;  // interleaved stereo, not yet queued
    uint64_t m_cframeDecoded = 0;
    bool m_fWav = false;

private:
    void ParseHeader()
    {
        if (m_ab.size() < 0x28) return;
        const auto it = std::search(m_ab.begin(), m_ab.end(), "SShd", "SShd" + 4);
        if (it == m_ab.end() || static_cast<std::size_t>(m_ab.end() - it) < 0x28) return;
        const uint8_t* h = &*it;
        m_format = Le32(h + 8);
        m_rate = Le32(h + 12);
        m_cch = Le32(h + 16);
        m_interleave = Le32(h + 20);
        const auto itBody = std::search(it + 8, m_ab.end(), "SSbd", "SSbd" + 4);
        if (itBody == m_ab.end() || static_cast<std::size_t>(m_ab.end() - itBody) < 8) return;
        if (m_cch < 1 || m_cch > 2 || m_rate == 0) return;
        if (m_interleave == 0) m_interleave = 0x800;
        m_ab.erase(m_ab.begin(), itBody + 8);
        m_fHeader = true;
    }

    void Decode()
    {
        const std::size_t cbUnit = std::size_t(m_interleave) * m_cch;
        std::size_t ib = 0;
        std::vector<int16_t> chan[2];
        while (m_ab.size() - ib >= cbUnit)
        {
            const std::size_t cframe = m_format == 0x10 || m_format == 2 ? m_interleave / 16 * 28 : m_interleave / 2;
            for (uint32_t c = 0; c < m_cch; ++c)
            {
                chan[c].resize(cframe);
                const uint8_t* p = m_ab.data() + ib + std::size_t(c) * m_interleave;
                if (m_format == 0x10 || m_format == 2)
                    for (std::size_t b = 0; b < m_interleave / 16; ++b) DecodeAdpcmBlock(p + b * 16, &chan[c][b * 28], &m_h1[c], &m_h2[c]);
                else
                    for (std::size_t i = 0; i < cframe; ++i)
                        chan[c][i] = static_cast<int16_t>(m_format == 1 ? (p[2 * i] | (p[2 * i + 1] << 8)) : ((p[2 * i] << 8) | p[2 * i + 1]));
            }
            for (std::size_t i = 0; i < cframe; ++i)
            {
                m_pcm.push_back(chan[0][i]);
                m_pcm.push_back(m_cch == 2 ? chan[1][i] : chan[0][i]);
            }
            m_cframeDecoded += cframe;
            ib += cbUnit;
        }
        m_ab.erase(m_ab.begin(), m_ab.begin() + static_cast<std::ptrdiff_t>(ib));
    }

    std::vector<uint8_t> m_ab;
    int m_h1[2] = {0, 0}, m_h2[2] = {0, 0};
    PFNWAVREAD m_pfnWav;
    WAVINFO m_wi;
    uint64_t m_iframeWav = 0;
};

struct PICTURE
{
    std::vector<uint8_t> rgba;
    double tDuration = 0.0;
};

std::string AvError(int error)
{
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, message, sizeof(message));
    return message;
}
}  // namespace

bool FDemuxMovie(const FK& fk, std::vector<uint8_t>& abVideo, std::map<int, MOVIEAUDIOTRACK>& mpaudio)
{
    CPssReader reader(fk);
    if (reader.FElementary()) return false;
    std::map<int, CMovieAudio> mpdec;
    uint32_t id = 0;
    const uint8_t* pb = nullptr;
    std::size_t cb = 0;
    // (as FPlayMovie demuxes: the video packets' bytes in order; the sound packets by their track, each decoded alone)
    while (reader.FNextPacket(&id, &pb, &cb))
    {
        if (id == 0xE0) abVideo.insert(abVideo.end(), pb, pb + cb);
        else if (cb >= 4) mpdec[pb[3]].AddPacket(pb, cb);
    }
    for (auto& [track, dec] : mpdec)
    {
        if (!dec.m_fHeader) continue;
        MOVIEAUDIOTRACK& t = mpaudio[track];
        t.rate = int(dec.m_rate);
        t.asample.assign(dec.m_pcm.begin(), dec.m_pcm.end());
    }
    return !abVideo.empty();
}

std::vector<MOVIEINFO> MovieList()
{
    std::vector<MOVIEINFO> a;
    for (const auto& [va, fk] : g_disc.ElfKeys())
    {
        uint8_t h[4];
        if (fk.cb < kSector || !g_disc.FReadFileBytes(fk.isector, 0, h, 4) || (Be32(h) != 0x000001BA && Be32(h) != 0x000001B3)) continue;
        a.push_back({va, fk});
    }
    return a;
}

bool FFindMovie(const std::string& arg, MOVIEINFO* pinfo)
{
    const std::vector<MOVIEINFO> a = MovieList();
    const unsigned long n = std::strtoul(arg.c_str(), nullptr, 0);
    for (const MOVIEINFO& m : a)
        if (m.fk.id == n || m.va == n)
        {
            *pinfo = m;
            return true;
        }
    if (n < a.size())
    {
        *pinfo = a[n];
        return true;
    }
    return false;
}

// orig: (EE) the movie player (0x1B2820..0x1B4728: libmpeg's decode loop, the IPU, the movie sound); Sly1-Release
// fmv.cpp PlayCutscene (the FFmpeg decoder, the frame pacing)
bool FPlayMovie(const FK& fk, PFNMOVIEPRESENT pfnPresent, void* pv, MOVIESTATS* pstats, double tMax)
{
    MOVIESTATS stats;
    av_log_set_level(AV_LOG_ERROR);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!codec) return false;
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    SwsContext* scaler = nullptr;
    bool fOk = decoder && parser && frame && packet;
    if (fOk)
    {
        decoder->thread_count = static_cast<int>(std::min(2u, std::max(1u, std::thread::hardware_concurrency())));
        decoder->thread_type = FF_THREAD_SLICE;
        fOk = avcodec_open2(decoder, codec, nullptr) >= 0;
    }

    CPssReader reader(fk);
    CMovieAudio audio;
    // (PC) an M2V's sound: the WAV of track 0 (one sound track: the USA disc), else the language's (track + 1)
    if (reader.FElementary())
    {
        std::string path = g_disc.PathPart(fk.isector, "audio0");
        if (path.empty()) path = g_disc.PathPart(fk.isector, "audio" + std::to_string(g_nLang + 1));
        if (!path.empty()) audio.FOpenWav(path);
    }
    CPcmStream pcm;
    std::deque<PICTURE> apicture;
    bool fEof = false, fQuit = false, fAudioStarted = false;
    double fps = 0.0;
    int cxSws = 0, cySws = 0;

    auto receiveFrames = [&] {
        while (fOk)
        {
            const int r = avcodec_receive_frame(decoder, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
            if (r < 0)
            {
                std::fprintf(stderr, "movie: decode: %s\n", AvError(r).c_str());
                return;
            }
            if (!scaler || cxSws != frame->width || cySws != frame->height)
            {
                sws_freeContext(scaler);
                cxSws = frame->width;
                cySws = frame->height;
                scaler = sws_getContext(cxSws, cySws, static_cast<AVPixelFormat>(frame->format), cxSws, cySws,
                                        AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            }
            if (fps <= 0.0)
            {
                fps = decoder->framerate.num > 0 && decoder->framerate.den > 0 ? av_q2d(decoder->framerate) : 29.97;
                if (fps <= 1.0 || fps > 120.0) fps = 29.97;
            }
            PICTURE pic;
            pic.rgba.resize(std::size_t(cxSws) * cySws * 4);
            uint8_t* dst[] = {pic.rgba.data()};
            int stride[] = {cxSws * 4};
            if (scaler) sws_scale(scaler, frame->data, frame->linesize, 0, cySws, dst, stride);
            pic.tDuration = (1.0 + frame->repeat_pict * 0.5) / fps;
            apicture.push_back(std::move(pic));
            stats.cx = cxSws;
            stats.cy = cySws;
        }
    };

    auto feedVideo = [&](const uint8_t* pb, std::size_t cb) {
        while (cb > 0 || pb == nullptr)
        {
            uint8_t* out = nullptr;
            int cbOut = 0;
            const int used = av_parser_parse2(parser, decoder, &out, &cbOut, pb, static_cast<int>(cb), AV_NOPTS_VALUE,
                                              AV_NOPTS_VALUE, 0);
            if (used < 0) return;
            if (pb)
            {
                pb += used;
                cb -= static_cast<std::size_t>(used);
            }
            if (cbOut > 0)
            {
                packet->data = out;
                packet->size = cbOut;
                if (avcodec_send_packet(decoder, packet) >= 0) receiveFrames();
            }
            if (!pb) break;
        }
    };

    // Reads packets until enough is buffered: two pictures and a second of sound (or the end).
    auto demux = [&] {
        audio.TopUpWav();
        // The audio header can follow the first video pictures. Keep reading until
        // it arrives (or the bounded video-only fallback below can start the clock).
        // Otherwise three pictures with no header satisfy neither startup condition.
        while (!fEof && (apicture.size() < 3 || !audio.m_fHeader || audio.m_pcm.size() < audio.m_rate * 2u))
        {
            uint32_t id = 0;
            const uint8_t* pb = nullptr;
            std::size_t cb = 0;
            if (!reader.FNextPacket(&id, &pb, &cb))
            {
                fEof = true;
                feedVideo(nullptr, 0);
                avcodec_send_packet(decoder, nullptr);
                receiveFrames();
                break;
            }
            if (id == 0xE0) feedVideo(pb, cb);
            // orig 0x1B3EC0: the sound streams with the id 0 (one sound track: the USA disc) or the language's + 1 (the
            // European discs: one track per language, 1..11; region.h)
            else if (cb >= 4 && (pb[3] == 0 || pb[3] == g_nLang + 1)) audio.AddPacket(pb, cb);
            if (apicture.size() > 8) break;  // (sound far behind the pictures: show some first)
        }
    };

    auto queueAudio = [&](bool fFlushAll) {
        if (!pcm.FIsOpen()) return;
        constexpr std::size_t kcframeChunk = 8192;
        while (pcm.FCanPush() && (audio.m_pcm.size() >= kcframeChunk * 2 || (fFlushAll && !audio.m_pcm.empty())))
        {
            const std::size_t cframe = std::min(kcframeChunk, audio.m_pcm.size() / 2);
            std::vector<int16_t> chunk(audio.m_pcm.begin(), audio.m_pcm.begin() + static_cast<std::ptrdiff_t>(cframe * 2));
            audio.m_pcm.erase(audio.m_pcm.begin(), audio.m_pcm.begin() + static_cast<std::ptrdiff_t>(cframe * 2));
            pcm.FPush(chunk.data(), cframe);
        }
    };

    using clock = std::chrono::steady_clock;
    auto tStart = clock::now();
    double tNext = 0.0;  // when the next picture is due (s since the start)
    uint64_t cframeQueuedAudio = 0;
    (void)cframeQueuedAudio;
    while (fOk && !fQuit)
    {
        demux();
        if (!fAudioStarted && (audio.m_fHeader || fEof || apicture.size() > 8))
        {
            if (std::getenv("SLY2N_MOVIEAUDIT"))
                std::printf("movie startup %u: pictures %zu, audio header %d, eof %d\n",
                            fk.id, apicture.size(), int(audio.m_fHeader), int(fEof));
            audio.TopUpWav();
            if (audio.m_fHeader && pcm.FOpen(2, static_cast<int>(audio.m_rate)))
            {
                // 989snd's movie voices (InitMovieSound vol 0x400 pan 0 group 2, type 3): one voice per channel,
                // each at MakeVolume(127, 0, 127, 0, 127, 0) through group 2, on its own side.
                float gl = 0.0f, gr = 0.0f;
                snd_StreamLevels(127, 0, 0, 2, &gl, &gr);
                const float m[4] = {gl * kStreamGain, 0.0f, 0.0f, gr * kStreamGain};
                pcm.SetMatrix(m);
                queueAudio(fEof && audio.FWavDone());
                pcm.Start();
            }
            fAudioStarted = true;
            tStart = clock::now();
        }
        if (!fAudioStarted) continue;
        audio.TopUpWav();
        queueAudio(fEof && audio.FWavDone());
        // The clock: the sound played (when there is sound), else the wall clock.
        const double tWall = std::chrono::duration<double>(clock::now() - tStart).count();
        const double t = pcm.FIsOpen() && pcm.CFramePlayed() > 0 ? double(pcm.CFramePlayed()) / audio.m_rate : tWall;
        if (t >= tMax) break;
        if (!apicture.empty() && t >= tNext)
        {
            PICTURE pic = std::move(apicture.front());
            apicture.pop_front();
            tNext += pic.tDuration;
            // (test: SLY2N_PCMTAP=dir: each picture's FNV-1a hash and duration, dir/movie-<id>.txt)
            if (static const char* s_szTap = std::getenv("SLY2N_PCMTAP"); s_szTap)
            {
                uint64_t h = 0xcbf29ce484222325ull;
                for (uint8_t b : pic.rgba) h = (h ^ b) * 0x100000001b3ull;
                const std::string path = std::string(s_szTap) + "/movie-" + std::to_string(fk.id) + ".txt";
                if (std::FILE* pf = std::fopen(path.c_str(), stats.cframe == 0 ? "w" : "a"))
                {
                    std::fprintf(pf, "%d %016llx %.6f\n", stats.cframe, static_cast<unsigned long long>(h), pic.tDuration);
                    std::fclose(pf);
                }
            }
            stats.cframe++;
            if (!pfnPresent(pic.rgba.data(), stats.cx, stats.cy, pv))
            {
                stats.fSkipped = true;
                fQuit = true;
            }
            continue;
        }
        if (apicture.empty() && fEof)
        {
            // The pictures are done: let the sound finish.
            if (!pcm.FIsOpen() || (audio.m_pcm.empty() && audio.FWavDone() && pcm.CBufferQueued() == 0)) break;
        }
        if (!pfnPresent(nullptr, 0, 0, pv))
        {
            stats.fSkipped = true;
            fQuit = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    stats.fps = fps;
    stats.tAudio = audio.m_rate ? double(audio.m_cframeDecoded) / audio.m_rate : 0.0;
    stats.audioRate = static_cast<int>(audio.m_rate);
    stats.audioChannels = static_cast<int>(audio.m_cch);
    stats.audioFormat = static_cast<int>(audio.m_format);
    stats.audioInterleave = static_cast<int>(audio.m_interleave);
    pcm.Close();
    sws_freeContext(scaler);
    av_packet_free(&packet);
    av_frame_free(&frame);
    if (parser) av_parser_close(parser);
    avcodec_free_context(&decoder);
    if (pstats) *pstats = stats;
    return fOk && stats.cframe > 0;
}

// ---------------------------------------------------------------------------------------------------------
// Commands

int ListMoviesCommand()
{
    int i = 0;
    for (const MOVIEINFO& m : MovieList())
    {
        uint8_t h[kSector];
        g_disc.FReadFileBytes(m.fk.isector, 0, h, sizeof(h));
        std::printf("%2d  key 0x%06X  id %5u  sector 0x%06X  %6.1f MB\n", i++, m.va, m.fk.id, m.fk.isector,
                    m.fk.cb / 1048576.0);
    }
    return 0;
}

namespace
{
struct SDLPRESENTER
{
    SDL_Window* pwindow = nullptr;
    SDL_Renderer* prenderer = nullptr;
    SDL_Texture* ptexture = nullptr;
    int cxTex = 0, cyTex = 0;
    std::string dirOut;
    double tLastShot = -10.0;
    std::chrono::steady_clock::time_point tStart = std::chrono::steady_clock::now();
    int cshot = 0;
};

bool PresentSdl(const uint8_t* prgba, int cx, int cy, void* pv)
{
    SDLPRESENTER& s = *static_cast<SDLPRESENTER*>(pv);
    if (s.pwindow)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT) return false;
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_RETURN ||
                                          e.key.keysym.sym == SDLK_SPACE))
                return false;
            if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_START) return false;
        }
    }
    if (!prgba) return true;
    if (s.prenderer)
    {
        if (!s.ptexture || s.cxTex != cx || s.cyTex != cy)
        {
            if (s.ptexture) SDL_DestroyTexture(s.ptexture);
            s.ptexture = SDL_CreateTexture(s.prenderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, cx, cy);
            s.cxTex = cx;
            s.cyTex = cy;
        }
        SDL_UpdateTexture(s.ptexture, nullptr, prgba, cx * 4);
        int cxWin = 0, cyWin = 0;
        SDL_GetRendererOutputSize(s.prenderer, &cxWin, &cyWin);
        // 4:3 (the PS2's display), letterboxed.
        const double aspect = 4.0 / 3.0;
        SDL_Rect r{0, 0, cxWin, cyWin};
        if (cxWin > cyWin * aspect)
        {
            r.w = static_cast<int>(cyWin * aspect);
            r.x = (cxWin - r.w) / 2;
        }
        else
        {
            r.h = static_cast<int>(cxWin / aspect);
            r.y = (cyWin - r.h) / 2;
        }
        SDL_SetRenderDrawColor(s.prenderer, 0, 0, 0, 255);
        SDL_RenderClear(s.prenderer);
        SDL_RenderCopy(s.prenderer, s.ptexture, nullptr, &r);
        SDL_RenderPresent(s.prenderer);
    }
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.tStart).count();
    if (!s.dirOut.empty() && t - s.tLastShot >= 2.0)
    {
        s.tLastShot = t;
        char name[64];
        std::snprintf(name, sizeof(name), "/frame-%03d.ppm", s.cshot++);
        std::ofstream f(s.dirOut + name, std::ios::binary);
        f << "P6\n" << cx << " " << cy << "\n255\n";
        for (int i = 0; i < cx * cy; ++i) f.write(reinterpret_cast<const char*>(prgba + 4 * i), 3);
    }
    return true;
}
}  // namespace

int PlayMovieCommand(const std::string& arg, const std::string& dirOut, double tMax)
{
    MOVIEINFO info;
    if (!FFindMovie(arg, &info))
    {
        std::fprintf(stderr, "no movie %s (see --list-movies)\n", arg.c_str());
        return 1;
    }
    SDLPRESENTER s;
    s.dirOut = dirOut;
    if (!dirOut.empty()) std::filesystem::create_directories(dirOut);
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) == 0)
    {
        s.pwindow = SDL_CreateWindow("Sly 2 movie", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (s.pwindow) s.prenderer = SDL_CreateRenderer(s.pwindow, -1, SDL_RENDERER_PRESENTVSYNC);
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
    }
    if (!s.prenderer) std::fprintf(stderr, "movie: no window (%s); playing without pictures\n", SDL_GetError());
    std::printf("movie id %u (key 0x%06X): sector 0x%06X, %.1f MB\n", info.fk.id, info.va, info.fk.isector,
                info.fk.cb / 1048576.0);
    MOVIESTATS stats;
    const bool f = FPlayMovie(info.fk, PresentSdl, &s, &stats, tMax);
    std::printf("%s: %dx%d at %.3f fps, %d pictures (%.1f s); sound format 0x%X, %d Hz, %d ch, interleave 0x%X, %.1f s%s\n",
                f ? "played" : "failed", stats.cx, stats.cy, stats.fps, stats.cframe,
                stats.fps > 0 ? stats.cframe / stats.fps : 0.0, stats.audioFormat, stats.audioRate,
                stats.audioChannels, stats.audioInterleave, stats.tAudio, stats.fSkipped ? " (skipped)" : "");
    if (s.ptexture) SDL_DestroyTexture(s.ptexture);
    if (s.prenderer) SDL_DestroyRenderer(s.prenderer);
    if (s.pwindow) SDL_DestroyWindow(s.pwindow);
    ShutdownAudio();
    return f ? 0 : 1;
}
