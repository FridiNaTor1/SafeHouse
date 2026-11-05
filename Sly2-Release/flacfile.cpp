// FLAC files (see flacfile.h), on libavcodec's FLAC encoder, parser and decoder.
#include "flacfile.h"

#include "../platform/platform.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
}

// FFmpeg 5.1 replaced channels / channel_layout with ch_layout, and 5.0 made the side data size a size_t. The Linux
// packages build against Ubuntu 22.04's FFmpeg 4.4 (PACKAGING.md), the Windows build against FFmpeg master.
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
#define FLAC_CH_LAYOUT 1
#else
#define FLAC_CH_LAYOUT 0
#endif
#if LIBAVCODEC_VERSION_MAJOR >= 59
using SideDataSize = std::size_t;
#else
using SideDataSize = int;
#endif

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace
{
constexpr std::size_t kcbStreamInfo = 34;
}

bool FIsFlac(const uint8_t* pb, std::size_t cb) { return cb >= 4 && !std::memcmp(pb, "fLaC", 4); }

bool FDecodeFlac(const std::vector<uint8_t>& ab, int& rate, int& cch, std::vector<int16_t>& asample, std::string* perror)
{
    auto fail = [&](const char* sz) { if (perror) *perror = sz; return false; };
    asample.clear();
    if (!FIsFlac(ab.data(), ab.size())) return fail("not a FLAC file");
    // the metadata blocks: STREAMINFO first (the decoder's extradata), then the frames
    std::size_t ib = 4;
    std::vector<uint8_t> abInfo;
    for (bool fLast = false; !fLast;)
    {
        if (ib + 4 > ab.size()) return fail("truncated metadata");
        fLast = (ab[ib] & 0x80) != 0;
        const int type = ab[ib] & 0x7F;
        const std::size_t cb = std::size_t(ab[ib + 1]) << 16 | std::size_t(ab[ib + 2]) << 8 | ab[ib + 3];
        if (ib + 4 + cb > ab.size()) return fail("truncated metadata");
        if (type == 0 && cb >= kcbStreamInfo) abInfo.assign(ab.begin() + std::ptrdiff_t(ib + 4), ab.begin() + std::ptrdiff_t(ib + 4 + kcbStreamInfo));
        ib += 4 + cb;
    }
    if (abInfo.empty()) return fail("no STREAMINFO");
    av_log_set_level(AV_LOG_ERROR);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_FLAC);
    if (!codec) return fail("no FLAC decoder in FFmpeg");
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_FLAC);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool fOk = ctx && parser && pkt && frame;
    if (fOk)
    {
        ctx->extradata = static_cast<uint8_t*>(av_mallocz(kcbStreamInfo + AV_INPUT_BUFFER_PADDING_SIZE));
        std::memcpy(ctx->extradata, abInfo.data(), kcbStreamInfo);
        ctx->extradata_size = int(kcbStreamInfo);
        fOk = avcodec_open2(ctx, codec, nullptr) >= 0;
    }
    rate = 0;
    cch = 0;
    auto receive = [&] {
        while (fOk && avcodec_receive_frame(ctx, frame) == 0)
        {
            rate = frame->sample_rate;
#if FLAC_CH_LAYOUT
            cch = frame->ch_layout.nb_channels;
#else
            cch = frame->channels;
#endif
            const auto fmt = static_cast<AVSampleFormat>(frame->format);
            const std::size_t i0 = asample.size();
            asample.resize(i0 + std::size_t(frame->nb_samples) * std::size_t(cch));
            for (int i = 0; i < frame->nb_samples; ++i)
                for (int k = 0; k < cch; ++k)
                {
                    int v = 0;
                    switch (fmt)
                    {
                    case AV_SAMPLE_FMT_S16: v = reinterpret_cast<const int16_t*>(frame->data[0])[i * cch + k]; break;
                    case AV_SAMPLE_FMT_S16P: v = reinterpret_cast<const int16_t*>(frame->data[k])[i]; break;
                    case AV_SAMPLE_FMT_S32: v = reinterpret_cast<const int32_t*>(frame->data[0])[i * cch + k] >> 16; break;
                    case AV_SAMPLE_FMT_S32P: v = reinterpret_cast<const int32_t*>(frame->data[k])[i] >> 16; break;
                    default: fOk = false; break;
                    }
                    asample[i0 + std::size_t(i) * std::size_t(cch) + std::size_t(k)] = int16_t(v);
                }
            av_frame_unref(frame);
        }
    };
    // the frames through the parser (it splits them), then the decoder
    std::vector<uint8_t> abFrames(ab.begin() + std::ptrdiff_t(ib), ab.end());
    abFrames.resize(abFrames.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    const uint8_t* p = abFrames.data();
    int cbLeft = int(ab.size() - ib);
    bool fFlush = false;  // (the input is all given: the parser is called empty until it has no frame left)
    while (fOk)
    {
        uint8_t* out = nullptr;
        int cbOut = 0;
        const int used = av_parser_parse2(parser, ctx, &out, &cbOut, fFlush ? nullptr : p, fFlush ? 0 : cbLeft, AV_NOPTS_VALUE,
                                          AV_NOPTS_VALUE, 0);
        if (used < 0) { fOk = false; break; }
        if (!fFlush)
        {
            p += used;
            cbLeft -= used;
        }
        if (cbOut > 0)
        {
            pkt->data = out;
            pkt->size = cbOut;
            if (avcodec_send_packet(ctx, pkt) < 0) { fOk = false; break; }
            receive();
        }
        else if (fFlush) break;
        if (cbLeft <= 0) fFlush = true;
    }
    if (fOk)
    {
        avcodec_send_packet(ctx, nullptr);
        receive();
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (parser) av_parser_close(parser);
    avcodec_free_context(&ctx);
    if (!fOk || cch <= 0 || rate <= 0) return fail("cannot decode");
    return true;
}

bool FLoadFlac(const std::string& path, int& rate, int& cch, std::vector<int16_t>& asample, std::string* perror)
{
    std::ifstream in(cane::path(path), std::ios::binary);
    if (!in)
    {
        if (perror) *perror = "cannot read";
        return false;
    }
    const std::vector<uint8_t> ab((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return FDecodeFlac(ab, rate, cch, asample, perror);
}

std::vector<uint8_t> AbFlac(int rate, int cch, const int16_t* asample, std::size_t cframe, std::string* perror)
{
    std::vector<uint8_t> abOut;
    auto fail = [&](const char* sz) { if (perror) *perror = sz; return std::vector<uint8_t>(); };
    av_log_set_level(AV_LOG_ERROR);
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!codec) return fail("no FLAC encoder in FFmpeg");
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool fOk = ctx && pkt && frame;
    if (fOk)
    {
        ctx->sample_fmt = AV_SAMPLE_FMT_S16;
        ctx->sample_rate = rate;
#if FLAC_CH_LAYOUT
        av_channel_layout_default(&ctx->ch_layout, cch);
#else
        ctx->channels = cch;
        ctx->channel_layout = uint64_t(av_get_default_channel_layout(cch));
#endif
        ctx->compression_level = 5;
        fOk = avcodec_open2(ctx, codec, nullptr) >= 0 && ctx->extradata_size >= int(kcbStreamInfo);
    }
    std::vector<uint8_t> abFrames, abInfo;
    if (fOk) abInfo.assign(ctx->extradata, ctx->extradata + kcbStreamInfo);
    auto drain = [&] {
        while (fOk && avcodec_receive_packet(ctx, pkt) == 0)
        {
            abFrames.insert(abFrames.end(), pkt->data, pkt->data + pkt->size);
            // (the last packet carries the final STREAMINFO: the sample count and the MD5)
            SideDataSize cbSide = 0;
            if (const uint8_t* pSide = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &cbSide); pSide && std::size_t(cbSide) >= kcbStreamInfo)
                abInfo.assign(pSide, pSide + kcbStreamInfo);
            av_packet_unref(pkt);
        }
    };
    const std::size_t cframeBlock = fOk && ctx->frame_size > 0 ? std::size_t(ctx->frame_size) : 4608;
    for (std::size_t i = 0; fOk && i < cframe; i += cframeBlock)
    {
        const std::size_t c = std::min(cframeBlock, cframe - i);
        frame->nb_samples = int(c);
        frame->format = AV_SAMPLE_FMT_S16;
        frame->sample_rate = rate;
#if FLAC_CH_LAYOUT
        av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
#else
        frame->channels = ctx->channels;
        frame->channel_layout = ctx->channel_layout;
#endif
        if (av_frame_get_buffer(frame, 0) < 0) { fOk = false; break; }
        std::memcpy(frame->data[0], asample + i * std::size_t(cch), c * std::size_t(cch) * sizeof(int16_t));
        fOk = avcodec_send_frame(ctx, frame) >= 0;
        av_frame_unref(frame);
        drain();
    }
    if (fOk)
    {
        avcodec_send_frame(ctx, nullptr);
        drain();
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    if (!fOk) return fail("cannot encode");
    abOut = {'f', 'L', 'a', 'C', 0x80, 0, 0, uint8_t(kcbStreamInfo)};  // STREAMINFO, the last metadata block
    abOut.insert(abOut.end(), abInfo.begin(), abInfo.end());
    abOut.insert(abOut.end(), abFrames.begin(), abFrames.end());
    return abOut;
}
