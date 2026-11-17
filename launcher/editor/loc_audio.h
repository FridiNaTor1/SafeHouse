#pragma once
// The Localisation tool's sound work for dubs (launcher/editor/loc_page.h's Voices view; desktop only): mono takes read
// and written as WAV, the radio effect (the lines the game plays as radio / binocucom talk: detected, its filter
// estimated from the reference line and put on a dub), trimming a recorded take. No ImGui, no device: the page records
// and plays; `safehouse --loc-selftest` checks the rules below (loc_core.h selfTest calls audioSelfTest).
//
// The radio effect. The game marks the lines its USB headset option plays (the dialog table's per-line flag:
// Sly2-Release/sound.h SNDSTREAM fNoHeadset == 0): the radio talk of Bentley and the gang on the binocucom. Those
// lines were mixed band-limited on the disc (no lows below ~300 Hz, little above ~3.5 kHz, compressed), the others are
// full range. A line is "radio" when the table flags it, else when its sound is band-limited that way (radioLike).
// The filter: the reference line's long-term spectrum (third-octave bands, its louder half) against the average
// spectrum of the disc's dry lines (kDrySpeech, measured on the USA English lines without the flag): the difference is
// the radio chain's EQ (band-pass and colour), smoothed and kept from boosting more than a little. A dub then gets that
// EQ (an STFT, the band gains per bin), the reference's compression (a level follower to its crest factor), a soft
// saturation and its noise floor (shaped noise under the speech at the reference's level).
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace cane::loc::audio {
namespace fs = std::filesystem;

// A mono take: samples in -1..1 at a rate.
struct Mono {
    int rate = 44100;
    std::vector<float> s;
    double seconds() const { return rate > 0 ? double(s.size()) / double(rate) : 0.0; }
    bool empty() const { return s.empty(); }
};

// ---- WAV
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
// A WAV's sound as mono (the channels averaged): PCM 8 / 16 / 24 / 32-bit or float 32; empty when not one.
inline Mono decodeWav(const std::vector<uint8_t>& d) {
    Mono out;
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(&d[8], "WAVE", 4) != 0) return out;
    int channels = 0, bits = 0, tag = 0;
    const uint8_t* data = nullptr;
    std::size_t size = 0;
    for (std::size_t at = 12; at + 8 <= d.size();) {
        const uint32_t n = rd32(&d[at + 4]);
        const std::size_t avail = std::min<std::size_t>(n, d.size() - at - 8);
        if (!std::memcmp(&d[at], "fmt ", 4) && avail >= 16) {
            tag = rd16(&d[at + 8]);
            channels = rd16(&d[at + 10]);
            out.rate = int(rd32(&d[at + 12]));
            bits = rd16(&d[at + 22]);
            if (tag == 0xFFFE && avail >= 26) tag = rd16(&d[at + 32]);   // (WAVE_FORMAT_EXTENSIBLE: its sub format)
        } else if (!std::memcmp(&d[at], "data", 4)) {
            data = &d[at + 8];
            size = avail;
        }
        at += 8 + std::size_t(n) + (n & 1);
    }
    if (!data || channels < 1 || out.rate <= 0 || (tag != 1 && tag != 3)) return {};
    const int bytes = bits / 8;
    if (bytes < 1 || bytes > 4 || (tag == 3 && bits != 32)) return {};
    const std::size_t frames = size / (std::size_t(bytes) * std::size_t(channels));
    out.s.resize(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        double sum = 0;
        for (int c = 0; c < channels; ++c) {
            const uint8_t* p = data + (f * std::size_t(channels) + std::size_t(c)) * std::size_t(bytes);
            double v;
            if (tag == 3) {
                float x;
                std::memcpy(&x, p, 4);
                v = x;
            } else if (bytes == 1) v = (int(p[0]) - 128) / 128.0;
            else if (bytes == 2) v = int16_t(rd16(p)) / 32768.0;
            else if (bytes == 3) v = double(int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8) / 8388608.0;
            else v = double(int32_t(rd32(p))) / 2147483648.0;
            sum += v;
        }
        out.s[f] = float(sum / channels);
    }
    return out;
}
inline Mono readWav(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decodeWav(d);
}
// 16-bit mono PCM (what the game and the import take)
inline std::vector<uint8_t> encodeWav(const Mono& m) {
    std::vector<uint8_t> d(44 + m.s.size() * 2);
    auto w32 = [&](std::size_t at, uint32_t v) { for (int k = 0; k < 4; ++k) d[at + std::size_t(k)] = uint8_t(v >> (8 * k)); };
    auto w16 = [&](std::size_t at, uint32_t v) { d[at] = uint8_t(v), d[at + 1] = uint8_t(v >> 8); };
    std::memcpy(&d[0], "RIFF", 4);
    w32(4, uint32_t(36 + m.s.size() * 2));
    std::memcpy(&d[8], "WAVEfmt ", 8);
    w32(16, 16), w16(20, 1), w16(22, 1), w32(24, uint32_t(m.rate)), w32(28, uint32_t(m.rate * 2)), w16(32, 2), w16(34, 16);
    std::memcpy(&d[36], "data", 4);
    w32(40, uint32_t(m.s.size() * 2));
    for (std::size_t i = 0; i < m.s.size(); ++i) {
        const int v = int(std::lround(std::clamp(m.s[i], -1.0f, 1.0f) * 32767.0f));
        w16(44 + i * 2, uint32_t(uint16_t(int16_t(v))));
    }
    return d;
}
inline bool writeWav(const fs::path& file, const Mono& m, std::string& error) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const auto d = encodeWav(m);
    const fs::path tmp = fs::path(file).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
        if (!out) { error = "cannot write " + tmp.string(); return false; }
    }
    fs::rename(tmp, file, ec);
    if (ec) { error = ec.message(); return false; }
    return true;
}

// ---- helpers
// Another rate (a windowed-sinc interpolation, 16 taps; band-limited to the lower Nyquist).
inline Mono resample(const Mono& m, int rate) {
    if (m.rate == rate || m.empty() || rate <= 0) return m;
    Mono o;
    o.rate = rate;
    const double ratio = double(m.rate) / double(rate);
    const double cut = std::min(1.0, 1.0 / ratio);   // (of the source's Nyquist)
    const std::size_t n = std::size_t(double(m.s.size()) / ratio);
    o.s.resize(n);
    const int taps = 16;
    const double pi = 3.14159265358979323846;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = double(i) * ratio;
        const long c = long(std::floor(x));
        double acc = 0, wsum = 0;
        for (long k = c - taps + 1; k <= c + taps; ++k) {
            const double t = x - double(k);
            const double a = pi * t * cut;
            const double sinc = std::abs(a) < 1e-9 ? 1.0 : std::sin(a) / a;
            const double win = 0.5 + 0.5 * std::cos(pi * t / taps);
            const double w = sinc * win * cut;
            if (k >= 0 && k < long(m.s.size())) acc += w * m.s[std::size_t(k)];
            wsum += w;
        }
        o.s[i] = float(wsum != 0 ? acc / wsum : 0.0);
    }
    return o;
}
inline double rms(const float* p, std::size_t n) {
    double e = 0;
    for (std::size_t i = 0; i < n; ++i) e += double(p[i]) * p[i];
    return n ? std::sqrt(e / double(n)) : 0.0;
}
inline float peak(const Mono& m) {
    float p = 0;
    for (float x : m.s) p = std::max(p, std::abs(x));
    return p;
}

// ---- FFT (radix 2, in place)
inline void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * 3.14159265358979323846 / double(len) * (inverse ? 1 : -1);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (std::size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= double(n);
}

// ---- the spectrum in third-octave bands
inline const std::vector<double>& bandCentres() {
    static const std::vector<double> c = {63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800,
                                          1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000};
    return c;
}
inline int bandCount() { return int(bandCentres().size()); }
// The average speech spectrum of the disc's dry lines (dB per band, the loudest band 0): the USA English lines the
// dialog tables do not flag for the headset, their louder half, measured by tools in this repo's history
// (docs/MODDING.md "Dubbing": how it was measured).
inline const std::vector<double>& drySpeech() {
    static const std::vector<double> d = {
#include "loc_dryspeech.inc"
    };
    return d;
}
struct Spectrum {
    std::vector<double> bandsDb;   // per band (dB, relative to the loudest band); empty when too short
    double crest = 0;              // peak / RMS of the louder half (compression lowers it)
    double floorDb = -90;          // the quietest tenth's level against the louder half's (dB)
};
// The long-term spectrum of a take: 2048-point frames (any rate), the louder half's power averaged per band.
inline Spectrum spectrum(const Mono& m) {
    Spectrum out;
    const std::size_t N = m.rate > 30000 ? 4096 : 2048, hop = N / 2;
    if (m.s.size() < N) return out;
    std::vector<double> win(N);
    for (std::size_t i = 0; i < N; ++i) win[i] = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * double(i) / double(N));
    std::vector<std::vector<double>> frames;
    std::vector<double> energy;
    std::vector<std::complex<double>> a(N);
    for (std::size_t at = 0; at + N <= m.s.size(); at += hop) {
        for (std::size_t i = 0; i < N; ++i) a[i] = m.s[at + i] * win[i];
        fft(a, false);
        std::vector<double> p(N / 2 + 1);
        double e = 0;
        for (std::size_t k = 0; k <= N / 2; ++k) p[k] = std::norm(a[k]), e += p[k];
        frames.push_back(std::move(p));
        energy.push_back(e);
    }
    std::vector<double> sorted = energy;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    const double low = sorted[sorted.size() / 10];
    std::vector<double> avg(N / 2 + 1, 0.0);
    int count = 0;
    double loudEnergy = 0;
    for (std::size_t f = 0; f < frames.size(); ++f)
        if (energy[f] >= median) {
            for (std::size_t k = 0; k <= N / 2; ++k) avg[k] += frames[f][k];
            loudEnergy += energy[f];
            ++count;
        }
    if (!count) return out;
    loudEnergy /= count;
    out.floorDb = 10 * std::log10((low + 1e-20) / (loudEnergy + 1e-20));
    const double binHz = double(m.rate) / double(N);
    out.bandsDb.assign(std::size_t(bandCount()), -120.0);
    double best = -1e9;
    for (int b = 0; b < bandCount(); ++b) {
        const double c = bandCentres()[std::size_t(b)];
        const double lo = c / std::pow(2.0, 1.0 / 6), hi = c * std::pow(2.0, 1.0 / 6);
        double e = 0;
        int n = 0;
        for (std::size_t k = 1; k <= N / 2; ++k) {
            const double f = double(k) * binHz;
            if (f >= lo && f < hi) e += avg[k] / count, ++n;
        }
        // (a band above the take's Nyquist: none)
        if (n == 0 || hi > m.rate / 2.0 * 1.12) { out.bandsDb[std::size_t(b)] = -120; continue; }
        out.bandsDb[std::size_t(b)] = 10 * std::log10(e + 1e-20);
        best = std::max(best, out.bandsDb[std::size_t(b)]);
    }
    for (double& x : out.bandsDb) x = x <= -119 ? -120 : x - best;
    // crest of the louder samples
    double pk = 0, e2 = 0;
    std::size_t n2 = 0;
    for (std::size_t f = 0; f < frames.size(); ++f)
        if (energy[f] >= median)
            for (std::size_t i = f * hop; i < f * hop + N && i < m.s.size(); ++i) pk = std::max(pk, double(std::abs(m.s[i]))), e2 += double(m.s[i]) * m.s[i], ++n2;
    out.crest = n2 && e2 > 0 ? pk / std::sqrt(e2 / double(n2)) : 0;
    return out;
}

// ---- the radio detector
// How a take's spectrum differs from dry speech the way the radio chain does, in dB against the dry average with the
// voice's body (400..2500 Hz) as the level both are compared at: the lows (63..200 Hz) and the highs (4..8 kHz) below
// the dry average's. {0, 0} for a dry line; the disc's flagged lines: lows ~30, highs ~5..15. {-1, -1}: too short.
struct RadioParts {
    double lows = -1, highs = -1;
};
inline RadioParts radioParts(const Spectrum& sp) {
    if (sp.bandsDb.empty()) return {};
    const auto& dry = drySpeech();
    auto level = [&](int from, int to, const std::vector<double>& v) {
        double e = 0;
        int n = 0;
        for (int b = from; b <= to; ++b)
            if (v[std::size_t(b)] > -119) e += std::pow(10.0, v[std::size_t(b)] / 10), ++n;
        return n ? 10 * std::log10(e / n + 1e-20) : -120.0;
    };
    const double body = level(8, 16, sp.bandsDb), bodyDry = level(8, 16, dry);
    return {(level(0, 5, dry) - bodyDry) - (level(0, 5, sp.bandsDb) - body), (level(18, 21, dry) - bodyDry) - (level(18, 21, sp.bandsDb) - body)};
}
// A band-limited take: the lows 20 dB and the highs 3 dB under dry speech. On the USA English lines (1691): 545 of the
// 592 the dialog tables flag for the headset (92%), and 136 of the 1099 others (the loudspeaker announcements, the
// recon talk heard through the binocucom, some shouts mixed thin), so the table's flag decides where there is one.
inline constexpr double kRadioLows = 20.0, kRadioHighs = 3.0;
inline bool radioLike(const Spectrum& sp) {
    const RadioParts p = radioParts(sp);
    return p.lows > kRadioLows && p.highs > kRadioHighs;
}

// ---- the filter
struct RadioFilter {
    std::vector<double> gainDb;   // per band
    double crest = 0;             // the reference's crest factor (0: no compression)
    double noiseDb = -90;         // its noise floor against the speech
    double drive = 0;             // saturation 0..1
    bool valid() const { return !gainDb.empty(); }
};
// The radio chain's EQ from a reference line: its spectrum minus the dry average, smoothed over three bands, its
// boosts kept to +6 dB, cuts to -40 dB, the body (400..2500 Hz) at 0 dB.
inline RadioFilter estimateFilter(const Spectrum& ref) {
    RadioFilter f;
    if (ref.bandsDb.empty()) return f;
    const auto& dry = drySpeech();
    std::vector<double> diff(static_cast<std::size_t>(bandCount()));
    for (int b = 0; b < bandCount(); ++b) {
        const double r = ref.bandsDb[std::size_t(b)];
        diff[std::size_t(b)] = r <= -119 ? -40 : r - dry[std::size_t(b)];
    }
    std::vector<double> sm(diff.size());
    for (std::size_t b = 0; b < diff.size(); ++b) {
        double s = 0, w = 0;
        for (int k = -1; k <= 1; ++k) {
            const long j = long(b) + k;
            if (j < 0 || j >= long(diff.size())) continue;
            const double wk = k == 0 ? 2 : 1;
            s += diff[std::size_t(j)] * wk, w += wk;
        }
        sm[b] = s / w;
    }
    double body = 0;
    for (int b = 8; b <= 16; ++b) body += sm[std::size_t(b)];
    body /= 9;
    f.gainDb.resize(sm.size());
    for (std::size_t b = 0; b < sm.size(); ++b) f.gainDb[b] = std::clamp(sm[b] - body, -40.0, 6.0);
    f.crest = ref.crest;
    f.noiseDb = std::clamp(ref.floorDb, -80.0, -25.0);
    f.drive = 0.35;
    return f;
}
// The filter's gain at a frequency (dB): the bands interpolated on a log axis
inline double gainAt(const RadioFilter& f, double hz) {
    const auto& c = bandCentres();
    if (hz <= c.front()) return f.gainDb.front();
    if (hz >= c.back()) return f.gainDb.back();
    for (std::size_t b = 0; b + 1 < c.size(); ++b)
        if (hz < c[b + 1]) {
            const double t = std::log(hz / c[b]) / std::log(c[b + 1] / c[b]);
            return f.gainDb[b] * (1 - t) + f.gainDb[b + 1] * t;
        }
    return f.gainDb.back();
}
// The EQ alone (an STFT: 1024 / 2048-point frames, half overlap, Hann analysis and synthesis windows squared to one)
inline Mono equalise(const Mono& in, const RadioFilter& f) {
    Mono out;
    out.rate = in.rate;
    const std::size_t N = in.rate > 30000 ? 2048 : 1024, hop = N / 4;
    out.s.assign(in.s.size(), 0.0f);
    if (in.s.empty()) return out;
    std::vector<double> win(N), gains(N / 2 + 1);
    for (std::size_t i = 0; i < N; ++i) win[i] = std::sqrt(0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * double(i) / double(N)));
    for (std::size_t k = 0; k <= N / 2; ++k) gains[k] = std::pow(10.0, gainAt(f, double(k) * in.rate / double(N)) / 20.0);
    // (hop N/4 with sqrt-Hann on both sides sums to 2: scaled back below)
    std::vector<double> acc(in.s.size() + 2 * N, 0.0);
    std::vector<std::complex<double>> a(N);
    for (long at = -long(N); at < long(in.s.size()); at += long(hop)) {
        for (std::size_t i = 0; i < N; ++i) {
            const long j = at + long(i);
            a[i] = (j >= 0 && j < long(in.s.size()) ? in.s[std::size_t(j)] : 0.0f) * win[i];
        }
        fft(a, false);
        for (std::size_t k = 0; k <= N / 2; ++k) {
            a[k] *= gains[k];
            if (k > 0 && k < N / 2) a[N - k] = std::conj(a[k]);
        }
        fft(a, true);
        for (std::size_t i = 0; i < N; ++i) {
            const long j = at + long(i);
            if (j >= 0 && j < long(in.s.size())) acc[std::size_t(j)] += a[i].real() * win[i];
        }
    }
    for (std::size_t i = 0; i < in.s.size(); ++i) out.s[i] = float(acc[i] / 2.0);
    return out;
}
// The whole chain on a dry take: EQ, compression toward the reference's crest factor, saturation, noise; the result
// at the dry take's peak level (at most -1 dBFS).
inline Mono applyRadio(const Mono& dry, const RadioFilter& f, uint32_t seed = 1) {
    if (!f.valid() || dry.empty()) return dry;
    const float peakIn = std::max(peak(dry), 1e-4f);
    Mono m = equalise(dry, f);
    // compression: a level follower (5 ms attack, 120 ms release) and a gain toward the reference's crest
    {
        const Spectrum sp = spectrum(m);
        const double target = f.crest > 1 ? f.crest : sp.crest;
        double ratio = 1;
        if (sp.crest > 1 && target > 1 && target < sp.crest) ratio = std::clamp(std::log(sp.crest) / std::log(target), 1.0, 6.0);
        if (ratio > 1.01) {
            const double att = std::exp(-1.0 / (0.005 * m.rate)), rel = std::exp(-1.0 / (0.120 * m.rate));
            double env = 0;
            const double ref = rms(m.s.data(), m.s.size()) * 1.5 + 1e-6;
            for (float& x : m.s) {
                const double a = std::abs(x);
                env = a > env ? att * env + (1 - att) * a : rel * env + (1 - rel) * a;
                if (env > ref) x = float(x * std::pow(env / ref, 1.0 / ratio - 1.0));
            }
        }
    }
    // saturation (tanh at a drive, level kept)
    {
        const float pk = std::max(peak(m), 1e-6f);
        const double k = 1 + 1.5 * f.drive;
        const double norm = std::tanh(k);
        for (float& x : m.s) x = float(std::tanh(k * x / pk) / norm * pk);
    }
    // noise at the reference's floor under the speech's louder level, through the same EQ
    // (only when the reference is noisier than the take: a take's own room noise is enough otherwise)
    if (f.noiseDb > -79 && f.noiseDb > spectrum(dry).floorDb + 3) {
        Mono n;
        n.rate = m.rate;
        n.s.resize(m.s.size());
        uint32_t r = seed * 2654435761u + 12345u;
        for (float& x : n.s) {
            r = r * 1664525u + 1013904223u;
            x = float((double(r >> 8) / double(1 << 24)) * 2 - 1);
        }
        n = equalise(n, f);
        // (the speech's louder half's RMS as the 0 dB the floor is against)
        std::vector<double> lv;
        const std::size_t w = std::size_t(m.rate / 50);
        for (std::size_t at = 0; at + w <= m.s.size(); at += w) lv.push_back(rms(&m.s[at], w));
        std::sort(lv.begin(), lv.end());
        const double loud = lv.empty() ? rms(m.s.data(), m.s.size()) : lv[lv.size() * 3 / 4];
        const double nr = rms(n.s.data(), n.s.size()) + 1e-12;
        const double g = loud * std::pow(10.0, f.noiseDb / 20.0) / nr;
        for (std::size_t i = 0; i < m.s.size(); ++i) m.s[i] += float(n.s[i] * g);
    }
    const float pk = std::max(peak(m), 1e-6f);
    const float want = std::min(peakIn, 0.891f);
    for (float& x : m.s) x *= want / pk;
    return m;
}

// ---- trimming a take
// The span [start, end) in seconds, with 8 ms fades at both cuts.
inline Mono trimmed(const Mono& m, double start, double end) {
    Mono o;
    o.rate = m.rate;
    const std::size_t a = std::size_t(std::clamp(start, 0.0, m.seconds()) * m.rate);
    const std::size_t b = std::size_t(std::clamp(end, 0.0, m.seconds()) * m.rate);
    if (b <= a) return o;
    o.s.assign(m.s.begin() + long(a), m.s.begin() + long(b));
    const std::size_t fade = std::min<std::size_t>(std::size_t(m.rate * 0.008), o.s.size() / 2);
    for (std::size_t i = 0; i < fade; ++i) {
        const float g = float(i) / float(fade);
        o.s[i] *= g;
        o.s[o.s.size() - 1 - i] *= g;
    }
    return o;
}
// Where the speech starts and ends (a suggestion for the trim handles): the first / last 10 ms window louder than
// -40 dB under the take's loudest window (or than 0.003), 60 ms kept around it.
inline std::pair<double, double> speechSpan(const Mono& m) {
    const std::size_t w = std::max<std::size_t>(1, std::size_t(m.rate / 100));
    if (m.s.size() < w) return {0.0, m.seconds()};
    std::vector<double> lv;
    for (std::size_t at = 0; at + w <= m.s.size(); at += w) lv.push_back(rms(&m.s[at], w));
    const double loudest = *std::max_element(lv.begin(), lv.end());
    const double thr = std::max(loudest * 0.01, 0.003);
    std::size_t first = lv.size(), last = 0;
    for (std::size_t i = 0; i < lv.size(); ++i)
        if (lv[i] > thr) first = std::min(first, i), last = i;
    if (first > last) return {0.0, m.seconds()};
    const double s = std::max(0.0, double(first) * double(w) / m.rate - 0.06);
    const double e = std::min(m.seconds(), double(last + 1) * double(w) / m.rate + 0.06);
    return {s, e};
}
// The min / max of each of `columns` pieces (a waveform's outline)
inline std::vector<std::pair<float, float>> outline(const Mono& m, int columns) {
    std::vector<std::pair<float, float>> out(std::size_t(std::max(columns, 1)), {0.0f, 0.0f});
    if (m.s.empty()) return out;
    for (std::size_t c = 0; c < out.size(); ++c) {
        const std::size_t a = m.s.size() * c / out.size(), b = std::max(a + 1, m.s.size() * (c + 1) / out.size());
        float lo = 0, hi = 0;
        for (std::size_t i = a; i < b && i < m.s.size(); ++i) lo = std::min(lo, m.s[i]), hi = std::max(hi, m.s[i]);
        out[c] = {lo, hi};
    }
    return out;
}

// ---- a recording in progress: the device's samples arrive on its thread (push), the page reads them (copy)
struct Recorder {
    int rate = 48000;
    double limitSeconds = 0;   // stops taking samples beyond this (0: none)
    std::vector<float> s;
    void reset(int r, double limit) { rate = r, limitSeconds = limit, s.clear(); }
    // (from the capture callback: mono floats)
    void push(const float* p, std::size_t n) {
        if (limitSeconds > 0) n = std::min(n, limit() > s.size() ? limit() - s.size() : 0);
        s.insert(s.end(), p, p + n);
    }
    std::size_t limit() const { return std::size_t(std::llround(limitSeconds * rate)); }
    bool full() const { return limitSeconds > 0 && s.size() >= limit(); }
    double seconds() const { return double(s.size()) / rate; }
};

// The rules above on made-up takes (0 when all pass); `printf`-style reporting by the caller's expect.
template <class Expect>
inline void audioSelfTest(Expect&& expect) {
    const double pi = 3.14159265358979323846;
    // a "voice": a buzz at 140 Hz with harmonics to 10 kHz, syllables 4 a second
    Mono dry;
    dry.rate = 44100;
    dry.s.resize(44100 * 2);
    uint32_t r = 7;
    for (std::size_t i = 0; i < dry.s.size(); ++i) {
        const double t = double(i) / dry.rate;
        double v = 0;
        for (int h = 1; h * 140 < 10000; ++h) v += std::sin(2 * pi * 140 * h * t + h) / std::sqrt(double(h));
        r = r * 1664525u + 1013904223u;
        v += 0.3 * ((double(r >> 8) / double(1 << 24)) * 2 - 1);
        const double env = 0.5 + 0.5 * std::sin(2 * pi * 4 * t);
        dry.s[i] = float(0.05 * v * env);
    }
    // (shaped like the disc's average dry line: its spectrum brought to drySpeech's)
    {
        const Spectrum raw = spectrum(dry);
        RadioFilter shape;
        shape.gainDb.resize(std::size_t(bandCount()));
        for (int b = 0; b < bandCount(); ++b) shape.gainDb[std::size_t(b)] = std::clamp(drySpeech()[std::size_t(b)] - raw.bandsDb[std::size_t(b)], -40.0, 40.0);
        dry = equalise(dry, shape);
        const float pk = peak(dry);
        for (float& x : dry.s) x *= 0.5f / pk;
    }
    // the WAV round trip
    {
        const Mono back = decodeWav(encodeWav(dry));
        bool same = back.s.size() == dry.s.size() && back.rate == dry.rate;
        for (std::size_t i = 0; same && i < dry.s.size(); i += 997) same = std::abs(back.s[i] - dry.s[i]) < 1.0f / 16000;
        expect(same, "a take's WAV round trip (16-bit mono)");
    }
    // a band-pass "radio" version of it: its score above the line, the dry one's under it
    RadioFilter bp;
    bp.gainDb.resize(std::size_t(bandCount()));
    for (int b = 0; b < bandCount(); ++b) {
        const double c = bandCentres()[std::size_t(b)];
        // (a smooth band-pass as a radio chain's: 12 dB a band under 400 Hz and over 3150 Hz)
        bp.gainDb[std::size_t(b)] = c < 400 ? std::max(-40.0, -12.0 * std::log2(400 / c) * 3) : c > 3150 ? std::max(-40.0, -12.0 * std::log2(c / 3150) * 3) : 0;
    }
    const Mono radio = equalise(dry, bp);
    const Spectrum sd = spectrum(dry), sr = spectrum(radio);
    expect(radioLike(sr) && !radioLike(sd), "the radio detector: band-passed is radio, full range is not");
    // the filter estimated from the radio take, put on the dry take: its spectrum close to the radio take's
    const RadioFilter est = estimateFilter(sr);
    const Spectrum se = spectrum(equalise(dry, est));
    double worst = 0;
    for (int b = 3; b < 21; ++b) worst = std::max(worst, std::abs((se.bandsDb[std::size_t(b)] - sd.bandsDb[std::size_t(b)]) - (sr.bandsDb[std::size_t(b)] - sd.bandsDb[std::size_t(b)])));
    expect(worst < 8.0, "the estimated filter follows the reference's band-pass (within 8 dB per band)");
    const Mono full = applyRadio(dry, est);
    expect(full.s.size() == dry.s.size() && radioLike(spectrum(full)) && peak(full) <= 0.9f, "the whole radio chain: as long, radio-like, no clipping");
    // trimming
    const Mono t = trimmed(dry, 0.5, 1.25);
    expect(std::abs(t.seconds() - 0.75) < 0.001 && std::abs(t.s.front()) < 1e-6f, "a trim: its length, faded in");
    Mono pad;
    pad.rate = 1000;
    pad.s.assign(3000, 0.0f);
    for (std::size_t i = 1000; i < 2000; ++i) pad.s[i] = float(0.5 * std::sin(double(i)));
    const auto span = speechSpan(pad);
    expect(std::abs(span.first - 0.94) < 0.02 && std::abs(span.second - 2.06) < 0.02, "the speech's span in a take with silence around");
    // the recorder stops at its limit
    Recorder rec;
    rec.reset(1000, 1.5);
    std::vector<float> chunk(400, 0.1f);
    for (int k = 0; k < 6; ++k) rec.push(chunk.data(), chunk.size());
    expect(rec.full() && rec.s.size() == 1500, "a recording stops at the reference's length and the margin");
    const Mono up = resample(pad, 2000);
    expect(up.s.size() == 6000 && up.rate == 2000, "resampling");
}

}  // namespace cane::loc::audio
