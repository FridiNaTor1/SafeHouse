// WAV files (see wavfile.h).
#include "wavfile.h"

#include "../platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
void Put32(std::vector<uint8_t>& ab, uint32_t v) { for (int i = 0; i < 4; ++i) ab.push_back(uint8_t(v >> (8 * i))); }
void Put16(std::vector<uint8_t>& ab, uint16_t v) { ab.push_back(uint8_t(v)); ab.push_back(uint8_t(v >> 8)); }
void Put4cc(std::vector<uint8_t>& ab, const char* sz) { ab.insert(ab.end(), sz, sz + 4); }
}  // namespace

bool FIsWav(const uint8_t* pb, std::size_t cb)
{
    return cb >= 12 && !std::memcmp(pb, "RIFF", 4) && !std::memcmp(pb + 8, "WAVE", 4);
}

bool FParseWav(const PFNWAVREAD& pfnRead, uint64_t cbFile, WAVINFO& info, std::string* perror)
{
    auto fail = [&](const char* sz) { if (perror) *perror = sz; return false; };
    info = WAVINFO{};
    uint8_t h[12];
    if (cbFile < 12 || !pfnRead(0, h, 12) || !FIsWav(h, 12)) return fail("not a WAV file");
    bool fFmt = false, fData = false;
    uint64_t cbData = 0;
    for (uint64_t i = 12; i + 8 <= cbFile;)
    {
        uint8_t ch[8];
        if (!pfnRead(i, ch, 8)) break;
        const uint32_t cb = Le32(ch + 4);
        const uint64_t ib = i + 8;
        const uint64_t cbHave = std::min<uint64_t>(cb, cbFile - ib);
        if (!std::memcmp(ch, "fmt ", 4) && cbHave >= 16)
        {
            uint8_t f[40] = {};
            if (!pfnRead(ib, f, std::size_t(std::min<uint64_t>(cbHave, sizeof(f))))) return fail("cannot read the format");
            info.format = Le16(f);
            info.cch = Le16(f + 2);
            info.rate = int(Le32(f + 4));
            info.bits = Le16(f + 14);
            if (info.format == 0xFFFE && cbHave >= 26) info.format = Le16(f + 24);  // (EXTENSIBLE: the sub-format)
            fFmt = true;
        }
        else if (!std::memcmp(ch, "data", 4))
        {
            info.ibData = ib;
            cbData = cbHave;
            fData = true;
        }
        else if (!std::memcmp(ch, "smpl", 4) && cbHave >= 36)
        {
            uint8_t s[36 + 24];
            const std::size_t cbRead = std::size_t(std::min<uint64_t>(cbHave, sizeof(s)));
            if (pfnRead(ib, s, cbRead))
            {
                info.fSmpl = true;
                if (Le32(s + 28) >= 1 && cbRead >= 60)
                {
                    info.fLoop = true;
                    info.iframeLoop = Le32(s + 36 + 8);
                    info.iframeLoopEnd = uint64_t(Le32(s + 36 + 12)) + 1;  // (the end is inclusive)
                }
            }
        }
        else if (!std::memcmp(ch, "stem", 4) && cbHave >= 8)
        {
            std::vector<uint8_t> s(std::size_t(std::min<uint64_t>(cbHave, 8 + 64 * 4)));
            if (pfnRead(ib, s.data(), s.size()) && Le32(s.data()) == 1)
            {
                const uint32_t c = std::min<uint32_t>(Le32(s.data() + 4), uint32_t((s.size() - 8) / 4));
                for (uint32_t k = 0; k < c; ++k)
                    info.astem.push_back({s[8 + k * 4], int8_t(s[9 + k * 4]), int16_t(Le16(&s[10 + k * 4]))});
            }
        }
        i = ib + cb + (cb & 1);
    }
    if (!fFmt || !fData || info.cch < 1 || info.rate <= 0) return fail("no audio data");
    if (!((info.format == 1 && (info.bits == 16 || info.bits == 24 || info.bits == 32)) || (info.format == 3 && info.bits == 32)))
        return fail("unsupported sample format (use 16-bit PCM, 24-bit PCM or 32-bit float)");
    info.cframe = cbData / (uint64_t(info.bits / 8) * info.cch);
    if (info.iframeLoop > info.cframe) info.fLoop = false;
    if (info.astem.size() != std::size_t(info.cch)) info.astem.clear();
    return true;
}

bool FReadWavFrames(const PFNWAVREAD& pfnRead, const WAVINFO& info, uint64_t iframe, std::size_t cframe, int16_t* asample)
{
    const std::size_t cs = cframe * std::size_t(info.cch);
    std::fill(asample, asample + cs, int16_t(0));
    if (iframe >= info.cframe) return true;
    const std::size_t cframeRead = std::size_t(std::min<uint64_t>(cframe, info.cframe - iframe));
    const int cbSample = info.bits / 8;
    const uint64_t ib = info.ibData + iframe * uint64_t(cbSample) * info.cch;
    const std::size_t csRead = cframeRead * std::size_t(info.cch);
    if (info.format == 1 && info.bits == 16)
    {
        if (!pfnRead(ib, asample, csRead * 2)) return false;
        // (the samples are little-endian; the hosts are too)
        return true;
    }
    std::vector<uint8_t> ab(csRead * std::size_t(cbSample));
    if (!pfnRead(ib, ab.data(), ab.size())) return false;
    for (std::size_t i = 0; i < csRead; ++i)
    {
        const uint8_t* p = ab.data() + i * std::size_t(cbSample);
        int v = 0;
        if (info.format == 3)
        {
            float f;
            std::memcpy(&f, p, 4);
            v = int(std::lround(std::clamp(f, -1.0f, 1.0f) * 32767.0f));
        }
        else if (info.bits == 24) v = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 16;
        else v = int32_t(Le32(p)) >> 16;
        asample[i] = int16_t(std::clamp(v, -32768, 32767));
    }
    return true;
}

PFNWAVREAD PfnReadFile(const std::string& path)
{
    return [path](uint64_t ib, void* pv, std::size_t cb) {
        std::ifstream in(cane::path(path), std::ios::binary);
        if (!in) return false;
        in.seekg(std::streamoff(ib));
        in.read(static_cast<char*>(pv), std::streamsize(cb));
        return std::size_t(in.gcount()) == cb;
    };
}

bool FLoadWav(const std::string& path, WAVINFO& info, std::vector<int16_t>& asample, std::string* perror)
{
#ifdef __SWITCH__
    // (the Switch: the size from the opened file: a stat is two more SD card requests, and a sound effect's first play
    // loads its sample here on the game thread)
    std::vector<uint8_t> ab;
    {
        std::ifstream in(cane::path(path), std::ios::binary | std::ios::ate);
        const std::streamoff cbFile = in ? std::streamoff(in.tellg()) : std::streamoff(-1);
        if (cbFile >= 0)
        {
            ab.resize(std::size_t(cbFile));
            in.seekg(0);
            in.read(reinterpret_cast<char*>(ab.data()), std::streamsize(cbFile));
        }
        if (cbFile < 0 || std::size_t(in.gcount()) != ab.size())
        {
            if (perror) *perror = "cannot read";
            return false;
        }
    }
    const uint64_t cb = ab.size();
#else
    std::error_code ec;
    const uint64_t cb = std::filesystem::file_size(cane::path(path), ec);
    if (ec)
    {
        if (perror) *perror = "cannot read";
        return false;
    }
    std::vector<uint8_t> ab(cb);
    {
        std::ifstream in(cane::path(path), std::ios::binary);
        in.read(reinterpret_cast<char*>(ab.data()), std::streamsize(cb));
        if (std::size_t(in.gcount()) != cb)
        {
            if (perror) *perror = "cannot read";
            return false;
        }
    }
#endif
    const PFNWAVREAD pfn = [&](uint64_t ib, void* pv, std::size_t c) {
        if (ib + c > ab.size()) return false;
        std::memcpy(pv, ab.data() + ib, c);
        return true;
    };
    if (!FParseWav(pfn, cb, info, perror)) return false;
    asample.resize(std::size_t(info.cframe) * std::size_t(info.cch));
    return FReadWavFrames(pfn, info, 0, std::size_t(info.cframe), asample.data());
}

std::vector<uint8_t> AbWav(int rate, int cch, const int16_t* asample, std::size_t cframe, const WAVINFO* ploop,
                           const std::vector<WAVSTEM>* pastem)
{
    std::vector<uint8_t> ab;
    const uint32_t cbData = uint32_t(cframe * std::size_t(cch) * 2);
    ab.reserve(cbData + 200);
    Put4cc(ab, "RIFF");
    Put32(ab, 0);  // (set below)
    Put4cc(ab, "WAVE");
    Put4cc(ab, "fmt ");
    const bool fExt = cch > 2;
    Put32(ab, fExt ? 40 : 16);
    Put16(ab, fExt ? 0xFFFE : 1);
    Put16(ab, uint16_t(cch));
    Put32(ab, uint32_t(rate));
    Put32(ab, uint32_t(rate) * uint32_t(cch) * 2);
    Put16(ab, uint16_t(cch * 2));
    Put16(ab, 16);
    if (fExt)
    {
        Put16(ab, 22);
        Put16(ab, 16);  // valid bits
        Put32(ab, 0);   // no speaker positions (the channels are stems)
        static const uint8_t s_abPcmGuid[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
        ab.insert(ab.end(), s_abPcmGuid, s_abPcmGuid + 16);
    }
    if (ploop && ploop->fSmpl)
    {
        Put4cc(ab, "smpl");
        Put32(ab, ploop->fLoop ? 60 : 36);
        Put32(ab, 0);                                      // manufacturer
        Put32(ab, 0);                                      // product
        Put32(ab, uint32_t(1000000000.0 / rate + 0.5));    // sample period (ns)
        Put32(ab, 60);                                     // MIDI unity note
        Put32(ab, 0);                                      // pitch fraction
        Put32(ab, 0);                                      // SMPTE format
        Put32(ab, 0);                                      // SMPTE offset
        Put32(ab, ploop->fLoop ? 1 : 0);                   // loops
        Put32(ab, 0);                                      // sampler data
        if (ploop->fLoop)
        {
            Put32(ab, 0);                                  // cue point id
            Put32(ab, 0);                                  // type: forward
            Put32(ab, uint32_t(ploop->iframeLoop));
            Put32(ab, uint32_t(ploop->iframeLoopEnd - 1)); // (inclusive)
            Put32(ab, 0);                                  // fraction
            Put32(ab, 0);                                  // play count: infinite
        }
    }
    if (pastem && !pastem->empty())
    {
        Put4cc(ab, "stem");
        Put32(ab, uint32_t(8 + pastem->size() * 4));
        Put32(ab, 1);
        Put32(ab, uint32_t(pastem->size()));
        for (const WAVSTEM& s : *pastem)
        {
            ab.push_back(s.vol);
            ab.push_back(uint8_t(s.id));
            Put16(ab, uint16_t(s.pan));
        }
    }
    Put4cc(ab, "data");
    Put32(ab, cbData);
    const std::size_t ib = ab.size();
    ab.resize(ib + cbData);
    std::memcpy(ab.data() + ib, asample, cbData);
    const uint32_t cbRiff = uint32_t(ab.size() - 8);
    std::memcpy(ab.data() + 4, &cbRiff, 4);
    return ab;
}

bool FWriteWavFile(const std::string& path, const std::vector<uint8_t>& ab)
{
    std::filesystem::create_directories(cane::path(path).parent_path());
    std::ofstream out(cane::path(path), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(ab.data()), std::streamsize(ab.size()));
    return bool(out);
}
