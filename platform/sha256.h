#pragma once

// SHA-256 for the installers' inventories and file checks (install.h hashFile; notes/UNRAVEL_2026-11-10.md "Install
// speed"). The same digests as picosha2, faster: the x86-64 SHA extensions when the CPU has them (every recent AMD and
// Intel desktop CPU: about 1.5-2 GB/s), else a plain 32-bit implementation (about 3x picosha2's byte-by-byte one).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#define CANE_SHA256_X86 1
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace cane {

namespace sha256detail {
alignas(16) inline constexpr uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void blocksPortable(uint32_t state[8], const uint8_t* p, size_t blocks) {
    uint32_t w[64];
    for (; blocks--; p += 64) {
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
}

#ifdef CANE_SHA256_X86
// The SHA extensions (Intel's published scheme): the state as ABEF / CDGH, 4 rounds per step, the message schedule
// with sha256msg1 / sha256msg2.
__attribute__((target("sha,sse4.1,ssse3"))) inline void blocksShaNi(uint32_t state[8], const uint8_t* p, size_t blocks) {
    const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[0]));
    __m128i state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[4]));
    tmp = _mm_shuffle_epi32(tmp, 0xB1);              // CDAB
    state1 = _mm_shuffle_epi32(state1, 0x1B);        // EFGH
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);   // ABEF
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);     // CDGH
    for (; blocks--; p += 64) {
        const __m128i abefSave = state0, cdghSave = state1;
        __m128i w[16];
        for (int i = 0; i < 4; ++i)
            w[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * i)), mask);
        for (int i = 0; i < 16; ++i) {
            if (i >= 4)
                w[i] = _mm_sha256msg2_epu32(_mm_add_epi32(_mm_sha256msg1_epu32(w[i - 4], w[i - 3]), _mm_alignr_epi8(w[i - 1], w[i - 2], 4)),
                                            w[i - 1]);
            __m128i msg = _mm_add_epi32(w[i], _mm_load_si128(reinterpret_cast<const __m128i*>(&k[4 * i])));
            state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
            msg = _mm_shuffle_epi32(msg, 0x0E);
            state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        }
        state0 = _mm_add_epi32(state0, abefSave);
        state1 = _mm_add_epi32(state1, cdghSave);
    }
    tmp = _mm_shuffle_epi32(state0, 0x1B);           // FEBA
    state1 = _mm_shuffle_epi32(state1, 0xB1);        // DCHG
    state0 = _mm_blend_epi16(tmp, state1, 0xF0);     // DCBA
    state1 = _mm_alignr_epi8(state1, tmp, 8);        // ABEF
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[0]), state0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[4]), state1);
}

inline bool cpuHasShaNi() {
    static const bool has = [] {
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
        const bool sse41 = (c >> 19) & 1, ssse3 = (c >> 9) & 1;
        if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
        return sse41 && ssse3 && ((b >> 29) & 1);
    }();
    return has;
}
#endif

inline void blocks(uint32_t state[8], const uint8_t* p, size_t n) {
#ifdef CANE_SHA256_X86
    if (cpuHasShaNi()) { blocksShaNi(state, p, n); return; }
#endif
    blocksPortable(state, p, n);
}
}  // namespace sha256detail

class Sha256 {
public:
    void update(const void* data, size_t cb) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total_ += cb;
        if (fill_) {
            const size_t n = std::min(cb, sizeof(buffer_) - fill_);
            std::memcpy(buffer_ + fill_, p, n);
            fill_ += n; p += n; cb -= n;
            if (fill_ < sizeof(buffer_)) return;
            sha256detail::blocks(state_, buffer_, 1);
            fill_ = 0;
        }
        if (cb >= 64) {
            sha256detail::blocks(state_, p, cb / 64);
            p += cb / 64 * 64;
            cb %= 64;
        }
        std::memcpy(buffer_, p, cb);
        fill_ = cb;
    }
    // The digest as 64 lowercase hex characters (picosha2's get_hash_hex_string).
    std::string hex() {
        uint8_t pad[72] = {0x80};
        const uint64_t bits = total_ * 8;
        const size_t padLen = (fill_ < 56 ? 56 - fill_ : 120 - fill_);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(pad, padLen);
        update(len, 8);
        static const char* const digits = "0123456789abcdef";
        std::string s;
        for (uint32_t v : state_)
            for (int i = 28; i >= 0; i -= 4) s += digits[(v >> i) & 15];
        return s;
    }

private:
    uint32_t state_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buffer_[64] = {};
    size_t fill_ = 0;
    uint64_t total_ = 0;
};

inline std::string sha256Hex(const void* data, size_t cb) {
    Sha256 h;
    h.update(data, cb);
    return h.hex();
}

}  // namespace cane
