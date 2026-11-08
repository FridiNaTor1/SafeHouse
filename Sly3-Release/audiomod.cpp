// Audio tools for mods (MODDING.md). They need no game data:
//
//   Sly2-Release --make-music OUT.wav IN.wav [--like ORIGINAL.wav|.vpk] [--sneak S.wav] [--alert A.wav]
//                [--loop SECONDS | --loop none]      (the installation's format: one WAV, its stems and loop in chunks)
//   Sly2-Release --make-vpk OUT.vpk IN.wav [the same options]   (the PS2 format, which the game still reads)
//   Sly2-Release --make-vag OUT.vag IN.wav
//   Sly2-Release --export-wav IN.wav|IN.vpk|IN.vag OUT.wav [--stem ID]
//
// A VPK is the game's music / ambience stream: " KPV" header (0x800 bytes), then the channels' SPU ADPCM interleaved by
// 0x1000 bytes. Its channel table gives each channel a stem id: 0 sneak (mono), 1 normal
// (stereo), 2 alert (stereo), 3 / 4 follow the characters, 5 plain. --like copies the replaced track's table, so the
// game's stem mixing keeps working: the normal WAV fills ids 1, 3, 4 and 5, --sneak id 0 (else the normal WAV, mixed to
// mono), --alert id 2 (else the normal WAV). Without --like the VPK is one plain stereo stem (id 5).
//
// The loop: the game loops music from the loop start (header +0x7FC, bytes per channel) to the end. --loop SECONDS sets
// it (default 0: the whole track), --loop none plays it once. The game goes on at the loop start's 28-sample block after
// the end, with the decoder's history carried over the jump, so: the start is delayed by under 28 samples of silence
// to put the loop start on a block, the loop body is resampled to a whole number of blocks (under 14 samples), and
// the loop's first block is encoded for both histories it is entered with. Checked: a sine looped this way has no
// step at the seam (tools: the second difference stays at the signal's own).
//
// A VAG is a dialog line: "VAGp" header (big-endian data size at +0xC, rate at +0x10), mono ADPCM from 0x30, a silent
// first block, the end flag on the last.
#include "audio.h"
#include "wavfile.h"

#include "../platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t kInterleave = 0x1000;
constexpr uint32_t kHeader = 0x800;

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void PutLe32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void PutBe32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i)); }

bool FReadAll(const std::string& path, std::vector<uint8_t>& ab)
{
    std::ifstream in(cane::path(path), std::ios::binary);
    if (!in) return false;
    ab.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
bool FWriteAll(const std::string& path, const std::vector<uint8_t>& ab)
{
    std::ofstream out(cane::path(path), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(ab.data()), std::streamsize(ab.size()));
    return bool(out);
}

// ------------------------------------------------------------------------------------------------ WAV

struct WAV
{
    int rate = 0;
    std::vector<std::vector<int16_t>> ach;  // the channels (1 or 2)
    [[nodiscard]] std::size_t Count() const { return ach.empty() ? 0 : ach[0].size(); }
    // channel i (a mono file's only channel for both)
    [[nodiscard]] const std::vector<int16_t>& Ch(int i) const { return ach[std::min<std::size_t>(i, ach.size() - 1)]; }
    [[nodiscard]] std::vector<int16_t> Mono() const
    {
        if (ach.size() == 1) return ach[0];
        std::vector<int16_t> m(Count());
        for (std::size_t i = 0; i < m.size(); ++i) m[i] = int16_t((int(ach[0][i]) + int(ach[1][i])) / 2);
        return m;
    }
};

// 16 / 24 / 32-bit PCM or 32-bit float, mono or stereo (more channels: the first two).
bool FReadWav(const std::string& path, WAV& wav, std::string& error)
{
    std::vector<uint8_t> ab;
    if (!FReadAll(path, ab)) { error = path + ": cannot read"; return false; }
    if (ab.size() < 12 || std::memcmp(ab.data(), "RIFF", 4) || std::memcmp(ab.data() + 8, "WAVE", 4)) { error = path + ": not a WAV file"; return false; }
    int fmt = 0, cch = 0, bits = 0;
    const uint8_t* pdata = nullptr;
    std::size_t cbData = 0;
    for (std::size_t i = 12; i + 8 <= ab.size();)
    {
        const uint32_t cb = Le32(ab.data() + i + 4);
        const uint8_t* p = ab.data() + i + 8;
        const std::size_t cbHave = std::min<std::size_t>(cb, ab.size() - i - 8);
        if (!std::memcmp(ab.data() + i, "fmt ", 4) && cbHave >= 16)
        {
            fmt = Le16(p);
            cch = Le16(p + 2);
            wav.rate = int(Le32(p + 4));
            bits = Le16(p + 14);
            if (fmt == 0xFFFE && cbHave >= 26) fmt = Le16(p + 24);   // (WAVE_FORMAT_EXTENSIBLE: the sub-format)
        }
        else if (!std::memcmp(ab.data() + i, "data", 4)) { pdata = p; cbData = cbHave; }
        i += 8 + cb + (cb & 1);
    }
    if (!pdata || cch < 1 || wav.rate <= 0) { error = path + ": no audio data"; return false; }
    if (!((fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32)))
    {
        error = path + ": unsupported sample format (use 16-bit PCM, 24-bit PCM or 32-bit float)";
        return false;
    }
    const int cbSample = bits / 8, cchUse = std::min(cch, 2);
    const std::size_t c = cbData / (std::size_t(cbSample) * cch);
    wav.ach.assign(cchUse, std::vector<int16_t>(c));
    for (std::size_t i = 0; i < c; ++i)
        for (int k = 0; k < cchUse; ++k)
        {
            const uint8_t* p = pdata + (i * cch + k) * cbSample;
            int v = 0;
            if (fmt == 3)
            {
                float f;
                std::memcpy(&f, p, 4);
                v = int(std::lround(std::clamp(f, -1.0f, 1.0f) * 32767.0f));
            }
            else if (bits == 16) v = int16_t(Le16(p));
            else if (bits == 24) v = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 16;
            else v = int32_t(Le32(p)) >> 16;
            wav.ach[k][i] = int16_t(std::clamp(v, -32768, 32767));
        }
    return true;
}

bool FWriteWav(const std::string& path, int rate, const std::vector<std::vector<int16_t>>& ach)
{
    const uint32_t cch = uint32_t(ach.size()), c = uint32_t(ach[0].size());
    std::vector<uint8_t> ab(44 + std::size_t(c) * cch * 2);
    std::memcpy(ab.data(), "RIFF", 4);
    PutLe32(ab.data() + 4, uint32_t(ab.size() - 8));
    std::memcpy(ab.data() + 8, "WAVEfmt ", 8);
    PutLe32(ab.data() + 16, 16);
    ab[20] = 1;
    ab[22] = uint8_t(cch);
    PutLe32(ab.data() + 24, uint32_t(rate));
    PutLe32(ab.data() + 28, uint32_t(rate) * cch * 2);
    ab[32] = uint8_t(cch * 2);
    ab[34] = 16;
    std::memcpy(ab.data() + 36, "data", 4);
    PutLe32(ab.data() + 40, c * cch * 2);
    for (uint32_t i = 0; i < c; ++i)
        for (uint32_t k = 0; k < cch; ++k)
        {
            const uint16_t v = uint16_t(ach[k][i]);
            ab[44 + (std::size_t(i) * cch + k) * 2] = uint8_t(v);
            ab[45 + (std::size_t(i) * cch + k) * 2] = uint8_t(v >> 8);
        }
    return FWriteAll(path, ab);
}

// ------------------------------------------------------------------------------------------------ SPU ADPCM

// One 16-byte block of 28 samples, with the decoder's own arithmetic (audio.cpp DecodeAdpcmBlock) so the history the
// encoder carries is what the game's decoder will have: every filter (0..4) and shift tried, the least squared error
// kept. A loop's first block is entered with two histories (the intro's end, the loop's end): it is chosen for both.
struct ENCODER
{
    int h1 = 0, h2 = 0;
    void Block(const int16_t* as, uint8_t* pb, uint8_t flags, const int* ph2nd = nullptr)
    {
        static constexpr int s_acoef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
        int64_t errBest = INT64_MAX;
        uint8_t abBest[16] = {};
        int h1Best = h1, h2Best = h2;
        for (int filter = 0; filter < 5; ++filter)
            for (int shift = 0; shift <= 12; ++shift)
            {
                uint8_t ab[16] = {};
                ab[0] = uint8_t(filter << 4 | shift);
                ab[1] = flags;
                int a = h1, b = h2;
                int a2 = ph2nd ? ph2nd[0] : 0, b2 = ph2nd ? ph2nd[1] : 0;
                int64_t err = 0;
                for (int i = 0; i < 28 && err < errBest; ++i)
                {
                    const int pred = (s_acoef[filter][0] * a + s_acoef[filter][1] * b + 32) >> 6;
                    const int pred2 = (s_acoef[filter][0] * a2 + s_acoef[filter][1] * b2 + 32) >> 6;
                    auto dec = [&](int nib, int p) { return std::clamp(((nib << 12) >> shift) + p, -32768, 32767); };
                    // the nibble nearest the target (from both histories: their mean prediction), then its neighbours
                    const int predUse = ph2nd ? (pred + pred2) / 2 : pred;
                    int nibble = std::clamp(int(std::lround(double(int(as[i]) - predUse) * double(1 << shift) / 4096.0)), -8, 7);
                    auto cost = [&](int nib) {
                        int64_t d = dec(nib, pred) - as[i], e = d * d;
                        if (ph2nd) { const int64_t d2 = dec(nib, pred2) - as[i]; e += d2 * d2; }
                        return e;
                    };
                    int64_t e = cost(nibble);
                    for (int nib : {nibble - 1, nibble + 1})
                        if (nib >= -8 && nib <= 7)
                            if (const int64_t e2 = cost(nib); e2 < e) { e = e2; nibble = nib; }
                    err += e;
                    ab[2 + i / 2] |= uint8_t((nibble & 0xF) << ((i & 1) * 4));
                    const int v = dec(nibble, pred), v2 = dec(nibble, pred2);
                    b = a; a = v;
                    b2 = a2; a2 = v2;
                }
                if (err < errBest)
                {
                    errBest = err;
                    std::memcpy(abBest, ab, 16);
                    h1Best = a;
                    h2Best = b;
                }
            }
        std::memcpy(pb, abBest, 16);
        h1 = h1Best;
        h2 = h2Best;
    }
};

// A channel's samples as ADPCM blocks (the sample count a multiple of 28), the last block carrying flagsLast. With a
// loop (iblockLoop), two passes: the second encodes the loop's first block for both the history it has from the intro
// and the one it gets from the loop's end (the game's decoder carries the history over the jump, as the SPU2's does).
std::vector<uint8_t> AbEncode(const std::vector<int16_t>& as, std::size_t iblockLoop, uint8_t flagsLast)
{
    const std::size_t cblock = as.size() / 28;
    std::vector<uint8_t> ab(cblock * 16);
    int ahEnd[2] = {0, 0};
    const bool fLoop = iblockLoop < cblock;
    for (int pass = 0; pass < (fLoop ? 2 : 1); ++pass)
    {
        ENCODER enc;
        for (std::size_t b = 0; b < cblock; ++b)
            enc.Block(as.data() + b * 28, ab.data() + b * 16, b + 1 == cblock ? flagsLast : 0, pass == 1 && b == iblockLoop ? ahEnd : nullptr);
        ahEnd[0] = enc.h1;
        ahEnd[1] = enc.h2;
    }
    return ab;
}

std::vector<int16_t> AsDecode(const uint8_t* pb, std::size_t cb)
{
    std::vector<int16_t> as(cb / 16 * 28);
    int h1 = 0, h2 = 0;
    for (std::size_t b = 0; b < cb / 16; ++b) DecodeAdpcmBlock(pb + b * 16, &as[b * 28], &h1, &h2);
    return as;
}

// ------------------------------------------------------------------------------------------------ the commands

struct CHANINFO { uint8_t vol = 127; int8_t id = 5; int16_t pan = 0; };

// The channel table of a track (--like): an installed music / ambience WAV's stem chunk, or a VPK's header.
bool FReadChannelTable(const std::string& like, std::vector<CHANINFO>& ach, std::string& error)
{
    std::vector<uint8_t> ab;
    if (!FReadAll(like, ab)) { error = like + ": cannot read"; return false; }
    if (FIsWav(ab.data(), ab.size()))
    {
        WAVINFO wi;
        const PFNWAVREAD pfn = [&](uint64_t ib, void* pv, std::size_t cb) { if (ib + cb > ab.size()) return false; std::memcpy(pv, ab.data() + ib, cb); return true; };
        if (!FParseWav(pfn, ab.size(), wi, &error)) { error = like + ": " + error; return false; }
        if (wi.astem.empty()) { error = like + ": has no stem table (a WAV of the installation's Music folder or an Ambience.wav has one)"; return false; }
        for (const WAVSTEM& st : wi.astem) ach.push_back({st.vol, st.id, st.pan});
        return true;
    }
    if (ab.size() < kHeader || std::memcmp(ab.data(), " KPV", 4)) { error = like + " is neither a VPK nor a WAV with stems"; return false; }
    const uint32_t nch = Le32(ab.data() + 0x14);
    if (nch == 0 || nch >= 6) { error = like + ": " + std::to_string(nch) + " channels"; return false; }
    for (uint32_t i = 0; i < nch; ++i)
        ach.push_back({ab[0x18 + i * 4], int8_t(ab[0x19 + i * 4]), int16_t(Le16(ab.data() + 0x1A + i * 4))});
    return true;
}

// Each channel's samples for a channel table: a stem's pair takes the WAV's left / right, a lone channel its mono mix;
// the sneak stem (0) --sneak's, the alert stem (2) --alert's, the rest the normal WAV's.
std::vector<std::vector<int16_t>> AasStems(const std::vector<CHANINFO>& ach, const WAV& wNormal, const WAV& wSneak, const WAV& wAlert)
{
    std::vector<std::vector<int16_t>> as(ach.size());
    for (std::size_t i = 0; i < ach.size(); ++i)
    {
        const WAV& w = ach[i].id == 0 && wSneak.Count() ? wSneak : ach[i].id == 2 && wAlert.Count() ? wAlert : wNormal;
        const bool fPairLeft = i + 1 < ach.size() && ach[i + 1].id == ach[i].id;
        const bool fPairRight = i > 0 && ach[i - 1].id == ach[i].id && !(i > 1 && ach[i - 2].id == ach[i].id);
        as[i] = fPairLeft ? w.Ch(0) : fPairRight ? w.Ch(1) : w.Mono();
    }
    std::size_t c = 0;
    for (const auto& a : as) c = std::max(c, a.size());
    for (auto& a : as) a.resize(c, 0);
    return as;
}

// --make-music: the installation's music / ambience format (wavfile.h): one WAV, a channel per stem channel, the stem
// table and the loop in its chunks. No block alignment: the game loops a WAV at any frame.
int MakeMusic(const std::string& out, const std::string& in, const std::string& like, const std::string& sneak,
              const std::string& alert, const std::string& loop)
{
    std::string error;
    WAV wNormal, wSneak, wAlert;
    if (!FReadWav(in, wNormal, error)) { std::fprintf(stderr, "make-music: %s\n", error.c_str()); return 1; }
    if (!sneak.empty() && !FReadWav(sneak, wSneak, error)) { std::fprintf(stderr, "make-music: %s\n", error.c_str()); return 1; }
    if (!alert.empty() && !FReadWav(alert, wAlert, error)) { std::fprintf(stderr, "make-music: %s\n", error.c_str()); return 1; }
    for (const WAV* p : {&wSneak, &wAlert})
        if (p->Count() && p->rate != wNormal.rate) { std::fprintf(stderr, "make-music: the stems' WAVs must have the same sample rate\n"); return 1; }
    std::vector<CHANINFO> ach;
    if (!like.empty()) { if (!FReadChannelTable(like, ach, error)) { std::fprintf(stderr, "make-music: %s\n", error.c_str()); return 1; } }
    else ach = {{127, 5, 270}, {127, 5, 90}};
    const std::vector<std::vector<int16_t>> as = AasStems(ach, wNormal, wSneak, wAlert);
    const std::size_t c = as[0].size();
    if (c == 0) { std::fprintf(stderr, "make-music: %s is empty\n", in.c_str()); return 1; }
    WAVINFO wl;
    wl.fSmpl = true;
    wl.fLoop = loop != "none";
    wl.iframeLoop = loop.empty() || loop == "none" ? 0 : std::size_t(std::max(0.0, std::atof(loop.c_str())) * wNormal.rate);
    wl.iframeLoopEnd = c;
    if (wl.fLoop && wl.iframeLoop >= c) { std::fprintf(stderr, "make-music: the loop start is past the end\n"); return 1; }
    std::vector<int16_t> aframe(c * ach.size());
    std::vector<WAVSTEM> astem;
    for (std::size_t i = 0; i < ach.size(); ++i)
    {
        astem.push_back({ach[i].vol, ach[i].id, ach[i].pan});
        for (std::size_t k = 0; k < c; ++k) aframe[k * ach.size() + i] = as[i][k];
    }
    if (!FWriteWavFile(out, AbWav(wNormal.rate, int(ach.size()), aframe.data(), c, &wl, &astem)))
    {
        std::fprintf(stderr, "make-music: cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("make-music: %s: %zu channels (stems", out.c_str(), ach.size());
    for (const CHANINFO& ci : ach) std::printf(" %d", ci.id);
    std::printf("), %d Hz, %.2f s, ", wNormal.rate, double(c) / wNormal.rate);
    if (wl.fLoop) std::printf("loops from %.3f s\n", double(wl.iframeLoop) / wNormal.rate);
    else std::printf("plays once\n");
    return 0;
}

int MakeVpk(const std::string& out, const std::string& in, const std::string& like, const std::string& sneak,
            const std::string& alert, const std::string& loop)
{
    std::string error;
    WAV wNormal, wSneak, wAlert;
    if (!FReadWav(in, wNormal, error)) { std::fprintf(stderr, "make-vpk: %s\n", error.c_str()); return 1; }
    if (!sneak.empty() && !FReadWav(sneak, wSneak, error)) { std::fprintf(stderr, "make-vpk: %s\n", error.c_str()); return 1; }
    if (!alert.empty() && !FReadWav(alert, wAlert, error)) { std::fprintf(stderr, "make-vpk: %s\n", error.c_str()); return 1; }
    for (const WAV* p : {&wSneak, &wAlert})
        if (p->Count() && p->rate != wNormal.rate) { std::fprintf(stderr, "make-vpk: the stems' WAVs must have the same sample rate\n"); return 1; }
    // the channel table: the replaced track's, else one plain stereo stem
    std::vector<CHANINFO> ach;
    if (!like.empty()) { if (!FReadChannelTable(like, ach, error)) { std::fprintf(stderr, "make-vpk: %s\n", error.c_str()); return 1; } }
    else ach = {{127, 5, 270}, {127, 5, 90}};
    std::vector<std::vector<int16_t>> as = AasStems(ach, wNormal, wSneak, wAlert);
    std::size_t c = 0;
    for (const auto& a : as) c = std::max(c, a.size());
    if (c < 28) { std::fprintf(stderr, "make-vpk: %s is too short\n", in.c_str()); return 1; }
    for (auto& a : as) a.resize(c, 0);
    // the loop: the game goes on at the loop start's block after the end, so both the loop start and the loop's
    // length must be whole blocks (28 samples). The start is delayed by under 28 samples of silence, and the loop body
    // resampled (cubic, wrapping around the loop) to the nearest whole block count: under 14 samples longer or shorter,
    // a pitch change of a few millionths for a loop of a minute.
    const bool fLoop = loop != "none";
    std::size_t iloop = 0;
    if (fLoop && !loop.empty()) iloop = std::size_t(std::max(0.0, std::atof(loop.c_str())) * wNormal.rate);
    if (iloop + 28 > c) { std::fprintf(stderr, "make-vpk: the loop start is past the end\n"); return 1; }
    std::size_t cpre = 0, cbodyOld = 0, cbody = 0;
    if (fLoop)
    {
        cpre = (28 - iloop % 28) % 28;
        cbodyOld = c - iloop;
        cbody = std::max<std::size_t>(28, (cbodyOld + 14) / 28 * 28);
        for (auto& a : as)
        {
            std::vector<int16_t> aNew(cpre, 0);
            aNew.insert(aNew.end(), a.begin(), a.begin() + std::ptrdiff_t(iloop));
            const int16_t* pb = a.data() + iloop;
            auto at = [&](std::ptrdiff_t k) { k %= std::ptrdiff_t(cbodyOld); if (k < 0) k += std::ptrdiff_t(cbodyOld); return double(pb[k]); };
            for (std::size_t k = 0; k < cbody; ++k)
            {
                if (cbody == cbodyOld) { aNew.push_back(pb[k]); continue; }
                const double x = double(k) * double(cbodyOld) / double(cbody);
                const std::ptrdiff_t i0 = std::ptrdiff_t(x);
                const double t = x - double(i0);
                const double y0 = at(i0 - 1), y1 = at(i0), y2 = at(i0 + 1), y3 = at(i0 + 2);
                const double v = y1 + 0.5 * t * (y2 - y0 + t * (2 * y0 - 5 * y1 + 4 * y2 - y3 + t * (3 * (y1 - y2) + y3 - y0)));
                aNew.push_back(int16_t(std::clamp(std::lround(v), -32768L, 32767L)));
            }
            a = std::move(aNew);
        }
        iloop += cpre;
    }
    else for (auto& a : as) a.resize((c + 27) / 28 * 28, 0);
    const std::size_t cblock = as[0].size() / 28;
    const uint32_t cbPer = uint32_t(cblock * 16);
    const uint32_t cchunk = (cbPer + kInterleave - 1) / kInterleave;
    std::vector<uint8_t> ab(kHeader + std::size_t(cchunk) * kInterleave * ach.size(), 0);
    std::memcpy(ab.data(), " KPV", 4);
    PutLe32(ab.data() + 4, cbPer);
    PutLe32(ab.data() + 8, kHeader);
    PutLe32(ab.data() + 0xC, 0x2000);
    PutLe32(ab.data() + 0x10, uint32_t(wNormal.rate));
    PutLe32(ab.data() + 0x14, uint32_t(ach.size()));
    for (std::size_t i = 0; i < ach.size(); ++i)
    {
        ab[0x18 + i * 4] = ach[i].vol;
        ab[0x19 + i * 4] = uint8_t(ach[i].id);
        ab[0x1A + i * 4] = uint8_t(uint16_t(ach[i].pan));
        ab[0x1B + i * 4] = uint8_t(uint16_t(ach[i].pan) >> 8);
    }
    PutLe32(ab.data() + 0x7FC, fLoop ? uint32_t(iloop / 28 * 16) : cbPer);
    for (std::size_t i = 0; i < ach.size(); ++i)
    {
        const std::vector<uint8_t> abCh = AbEncode(as[i], fLoop ? iloop / 28 : SIZE_MAX, 0);
        for (uint32_t k = 0; k < cchunk; ++k)
        {
            const uint32_t cb = std::min(kInterleave, cbPer - k * kInterleave);
            std::memcpy(ab.data() + kHeader + (std::size_t(k) * ach.size() + i) * kInterleave, abCh.data() + std::size_t(k) * kInterleave, cb);
        }
    }
    if (!FWriteAll(out, ab)) { std::fprintf(stderr, "make-vpk: cannot write %s\n", out.c_str()); return 1; }
    std::printf("make-vpk: %s: %zu channels (stems", out.c_str(), ach.size());
    for (const CHANINFO& ci : ach) std::printf(" %d", ci.id);
    std::printf("), %d Hz, %.2f s, ", wNormal.rate, double(cblock * 28) / wNormal.rate);
    if (fLoop)
    {
        std::printf("loops from %.3f s", double(iloop - cpre) / wNormal.rate);
        if (cpre || cbody != cbodyOld) std::printf(" (start delayed %zu samples, loop %zu -> %zu samples)", cpre, cbodyOld, cbody);
        std::printf("\n");
    }
    else std::printf("plays once\n");
    return 0;
}

int MakeVag(const std::string& out, const std::string& in)
{
    std::string error;
    WAV w;
    if (!FReadWav(in, w, error)) { std::fprintf(stderr, "make-vag: %s\n", error.c_str()); return 1; }
    std::vector<int16_t> as = w.Mono();
    as.resize((as.size() + 27) / 28 * 28, 0);
    // a silent first block (as the game's lines have), then the line, the end flag on the last block
    std::vector<int16_t> asAll(28, 0);
    asAll.insert(asAll.end(), as.begin(), as.end());
    const std::vector<uint8_t> abData = AbEncode(asAll, SIZE_MAX, 1);
    std::vector<uint8_t> ab(0x30, 0);
    std::memcpy(ab.data(), "VAGp", 4);
    PutBe32(ab.data() + 0xC, uint32_t(abData.size()));
    PutBe32(ab.data() + 0x10, uint32_t(w.rate));
    ab.insert(ab.end(), abData.begin(), abData.end());
    if (!FWriteAll(out, ab)) { std::fprintf(stderr, "make-vag: cannot write %s\n", out.c_str()); return 1; }
    std::printf("make-vag: %s: mono, %d Hz, %.2f s%s\n", out.c_str(), w.rate, double(as.size()) / w.rate,
                w.rate != 22050 ? " (the game's lines are 22050 Hz; other rates play too)" : "");
    return 0;
}

int ExportWav(const std::string& in, const std::string& out, int stem)
{
    std::vector<uint8_t> ab;
    if (!FReadAll(in, ab) || ab.size() < 0x40) { std::fprintf(stderr, "export-wav: cannot read %s\n", in.c_str()); return 1; }
    if (FIsWav(ab.data(), ab.size()))
    {
        // an installed WAV: a music / ambience track's stem (its channels, with the loop), else the file as it is
        WAVINFO wi;
        std::vector<int16_t> a;
        std::string error;
        if (!FLoadWav(in, wi, a, &error)) { std::fprintf(stderr, "export-wav: %s: %s\n", in.c_str(), error.c_str()); return 1; }
        std::vector<int> aich;
        if (!wi.astem.empty())
        {
            if (stem < 0)
            {
                stem = wi.astem[0].id;
                for (const WAVSTEM& st : wi.astem) if (st.id == 1) stem = 1;
            }
            for (std::size_t i = 0; i < wi.astem.size(); ++i) if (wi.astem[i].id == stem) aich.push_back(int(i));
            if (aich.empty()) { std::fprintf(stderr, "export-wav: %s has no stem %d\n", in.c_str(), stem); return 1; }
        }
        else for (int i = 0; i < wi.cch; ++i) aich.push_back(i);
        std::vector<int16_t> o(std::size_t(wi.cframe) * aich.size());
        for (std::size_t k = 0; k < wi.cframe; ++k)
            for (std::size_t j = 0; j < aich.size(); ++j) o[k * aich.size() + j] = a[k * std::size_t(wi.cch) + std::size_t(aich[j])];
        if (!FWriteWavFile(out, AbWav(wi.rate, int(aich.size()), o.data(), std::size_t(wi.cframe), &wi)))
        {
            std::fprintf(stderr, "export-wav: cannot write %s\n", out.c_str());
            return 1;
        }
        std::printf("export-wav: %s: %zu channels%s, %d Hz, %.2f s, ", out.c_str(), aich.size(),
                    wi.astem.empty() ? "" : (" (stem " + std::to_string(stem) + ")").c_str(), wi.rate, double(wi.cframe) / wi.rate);
        if (wi.fLoop) std::printf("loop start %.3f s\n", double(wi.iframeLoop) / wi.rate);
        else std::printf("%s\n", wi.fSmpl ? "no loop" : "loops whole (no smpl chunk)");
        if (!wi.astem.empty())
        {
            std::printf("export-wav: the file's stems:");
            for (const WAVSTEM& st : wi.astem) std::printf(" %d", st.id);
            std::printf("\n");
        }
        return 0;
    }
    if (!std::memcmp(ab.data(), "VAGp", 4))
    {
        const uint32_t cb = std::min<uint32_t>(Be32(ab.data() + 0xC), uint32_t(ab.size() - 0x30));
        const int rate = int(Be32(ab.data() + 0x10));
        if (!FWriteWav(out, rate, {AsDecode(ab.data() + 0x30, cb)})) { std::fprintf(stderr, "export-wav: cannot write %s\n", out.c_str()); return 1; }
        std::printf("export-wav: %s: mono, %d Hz, %.2f s\n", out.c_str(), rate, cb / 16 * 28.0 / rate);
        return 0;
    }
    if (std::memcmp(ab.data(), " KPV", 4) || ab.size() < kHeader) { std::fprintf(stderr, "export-wav: %s is neither a VPK nor a VAG\n", in.c_str()); return 1; }
    const uint32_t cbPer = Le32(ab.data() + 4), data = Le32(ab.data() + 8), rate = Le32(ab.data() + 0x10), nch = Le32(ab.data() + 0x14);
    const uint32_t loop = Le32(ab.data() + 0x7FC);
    if (nch == 0 || nch >= 6 || rate == 0) { std::fprintf(stderr, "export-wav: %s: bad header\n", in.c_str()); return 1; }
    std::vector<int> aid(nch);
    for (uint32_t i = 0; i < nch; ++i) aid[i] = int8_t(ab[0x19 + i * 4]);
    // the stem: asked, else the normal one (1), else the first
    if (stem < 0) stem = std::count(aid.begin(), aid.end(), 1) ? 1 : aid[0];
    std::vector<std::vector<int16_t>> ach;
    for (uint32_t i = 0; i < nch; ++i)
    {
        if (aid[i] != stem) continue;
        std::vector<uint8_t> abCh(cbPer);
        for (uint32_t k = 0; k * kInterleave < cbPer; ++k)
        {
            const std::size_t ib = data + (std::size_t(k) * nch + i) * kInterleave;
            const uint32_t cb = std::min(kInterleave, cbPer - k * kInterleave);
            if (ib + cb > ab.size()) { std::fprintf(stderr, "export-wav: %s is truncated\n", in.c_str()); return 1; }
            std::memcpy(abCh.data() + std::size_t(k) * kInterleave, ab.data() + ib, cb);
        }
        ach.push_back(AsDecode(abCh.data(), abCh.size()));
    }
    if (ach.empty()) { std::fprintf(stderr, "export-wav: %s has no stem %d\n", in.c_str(), stem); return 1; }
    if (!FWriteWav(out, int(rate), ach)) { std::fprintf(stderr, "export-wav: cannot write %s\n", out.c_str()); return 1; }
    std::printf("export-wav: %s: stem %d (%zu channels), %u Hz, %.2f s, ", out.c_str(), stem, ach.size(), rate, cbPer / 16 * 28.0 / rate);
    if (loop < cbPer) std::printf("loop start %.3f s\n", loop / 16 * 28.0 / rate);
    else std::printf("no loop\n");
    std::printf("export-wav: the file's stems:");
    for (int id : aid) std::printf(" %d", id);
    std::printf("\n");
    return 0;
}
}  // namespace

// The audio tools' command line (main.cpp, before anything else: no game data needed). -1: not an audio command.
int AudioToolMain(int argc, char** argv)
{
    std::string cmd, a, b, like, sneak, alert, loop;
    int stem = -1;
    for (int i = 1; i < argc; ++i)
    {
        const std::string s = argv[i];
        if ((s == "--make-vpk" || s == "--make-vag" || s == "--export-wav" || s == "--make-music") && i + 2 < argc) { cmd = s; a = argv[++i]; b = argv[++i]; }
        else if (s == "--like" && i + 1 < argc) like = argv[++i];
        else if (s == "--sneak" && i + 1 < argc) sneak = argv[++i];
        else if (s == "--alert" && i + 1 < argc) alert = argv[++i];
        else if (s == "--loop" && i + 1 < argc) loop = argv[++i];
        else if (s == "--stem" && i + 1 < argc) stem = std::atoi(argv[++i]);
    }
    if (cmd == "--make-vpk") return MakeVpk(a, b, like, sneak, alert, loop);
    if (cmd == "--make-music") return MakeMusic(a, b, like, sneak, alert, loop);
    if (cmd == "--make-vag") return MakeVag(a, b);
    if (cmd == "--export-wav") return ExportWav(a, b, stem);
    return -1;
}
