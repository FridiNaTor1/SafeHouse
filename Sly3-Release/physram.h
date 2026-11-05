#pragma once

// Reading the reference's EE RAM snapshots (SLY2_DUMP_CALL / SLY2_DUMP_AT) for the physics checks (physcheck.cpp):
// the original's objects at their addresses, turned into native objects.

#include <glm/glm.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct RAMIMG
{
    std::vector<uint8_t> ab;
    uint64_t agpr[32][2]{};  // the regs file: 32 GPRs (low, high u64)
    uint32_t afpr[32]{};
    float avf[32][4]{};

    bool FLoad(const std::string& pathRam, const std::string& pathRegs)
    {
        std::FILE* pf = std::fopen(pathRam.c_str(), "rb");
        if (!pf) return false;
        ab.resize(32u << 20);
        const size_t cb = std::fread(ab.data(), 1, ab.size(), pf);
        std::fclose(pf);
        if (cb != ab.size()) return false;
        if (!pathRegs.empty())
        {
            pf = std::fopen(pathRegs.c_str(), "rb");
            if (!pf) return false;
            bool f = std::fread(agpr, 1, sizeof agpr, pf) == sizeof agpr;
            f = f && std::fread(afpr, 1, sizeof afpr, pf) == sizeof afpr;
            f = f && std::fread(avf, 1, sizeof avf, pf) == sizeof avf;
            std::fclose(pf);
            return f;
        }
        return true;
    }
    bool FValid(uint32_t a, uint32_t cb = 4) const { return a >= 0x1000 && uint64_t(a) + cb <= ab.size(); }
    template <class T>
    T Rd(uint32_t a) const
    {
        T t{};
        if (FValid(a, sizeof(T))) std::memcpy(&t, &ab[a], sizeof(T));
        return t;
    }
    uint32_t U32(uint32_t a) const { return Rd<uint32_t>(a); }
    int32_t S32(uint32_t a) const { return Rd<int32_t>(a); }
    int16_t S16(uint32_t a) const { return Rd<int16_t>(a); }
    uint16_t U16(uint32_t a) const { return Rd<uint16_t>(a); }
    uint64_t U64(uint32_t a) const { return Rd<uint64_t>(a); }
    float G(uint32_t a) const { return Rd<float>(a); }
    glm::vec3 V3(uint32_t a) const { return glm::vec3(G(a), G(a + 4), G(a + 8)); }
    glm::vec4 V4(uint32_t a) const { return glm::vec4(G(a), G(a + 4), G(a + 8), G(a + 12)); }
    // A MATRIX4 (4 rows of 4 floats) as glm (columns = rows)
    glm::mat4 M4(uint32_t a) const
    {
        glm::mat4 m;
        for (int i = 0; i < 4; i++) m[i] = V4(a + 16 * i);
        return m;
    }
    glm::mat3 M3(uint32_t a) const
    {
        glm::mat3 m;
        for (int i = 0; i < 3; i++) m[i] = V3(a + 16 * i);
        return m;
    }
    uint32_t A0() const { return static_cast<uint32_t>(agpr[4][0]); }
    uint32_t A1() const { return static_cast<uint32_t>(agpr[5][0]); }
    uint32_t A2() const { return static_cast<uint32_t>(agpr[6][0]); }
    uint32_t A3() const { return static_cast<uint32_t>(agpr[7][0]); }
    uint32_t Reg(int r) const { return static_cast<uint32_t>(agpr[r][0]); }
    glm::vec4 QReg(int r) const
    {
        glm::vec4 v;
        std::memcpy(&v, agpr[r], 16);
        return v;
    }
    float F(int r) const
    {
        float f;
        std::memcpy(&f, &afpr[r], 4);
        return f;
    }
    // The update flags at +0x40 (tagged: bit 0 clear = a pointer to the flags word of the world entry)
    uint32_t GrfTagged(uint32_t p) const
    {
        const uint32_t v = U32(p + 0x40);
        return (v & 1) ? v : U32(v);
    }
};

// Comparison statistics of one quantity.
struct DIFFSTAT
{
    std::string name;
    int c = 0, cbad = 0;
    double gMax = 0.0;
    void Add(double d, double tol)
    {
        ++c;
        if (d > gMax) gMax = d;
        if (d > tol) ++cbad;
    }
};
inline double DiffRel(float a, float b)
{
    const double d = std::abs(double(a) - double(b));
    const double m = std::max(1.0, std::max(std::abs(double(a)), std::abs(double(b))));
    return d / m;
}
