#pragma once

// Reference RAM snapshots (EE RAM dumps of Sly2-Reference, the original code) and the mapping of their objects to the
// native world's, for the differential checks (animref.cpp: --anim-ref / --anim-run / --anim-pose; airef.cpp:
// --ai-ref).

#include "lo.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

struct SW;

constexpr uint32_t REF_RAM_SIZE = 0x2000000;
constexpr uint32_t REF_SW_ADDR = 0x3DD490;

struct RAM
{
    std::vector<uint8_t> ab;
    bool FLoad(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        ab.assign(std::istreambuf_iterator<char>(f), {});
        return ab.size() >= REF_RAM_SIZE;
    }
    bool FValid(uint32_t a) const { return a && a + 4 <= REF_RAM_SIZE; }
    uint32_t U32(uint32_t a) const { uint32_t u = 0; if (FValid(a)) std::memcpy(&u, &ab[a], 4); return u; }
    int32_t S32(uint32_t a) const { return static_cast<int32_t>(U32(a)); }
    int16_t S16(uint32_t a) const { int16_t s = 0; if (FValid(a)) std::memcpy(&s, &ab[a], 2); return s; }
    uint16_t U16(uint32_t a) const { return static_cast<uint16_t>(S16(a)); }
    float F32(uint32_t a) const { float g = 0; if (FValid(a)) std::memcpy(&g, &ab[a], 4); return g; }
    glm::vec4 Vec4(uint32_t a) const { return glm::vec4(F32(a), F32(a + 4), F32(a + 8), F32(a + 12)); }
    glm::mat4 Mat4(uint32_t a) const
    {
        glm::mat4 m;
        for (int i = 0; i < 4; ++i) m[i] = Vec4(a + 16 * i);
        return m;
    }
    // a DL {head, tail, ibDle}
    std::vector<uint32_t> Dl(uint32_t pdl) const
    {
        std::vector<uint32_t> a;
        const uint32_t ib = U32(pdl + 8);
        for (uint32_t p = U32(pdl); FValid(p) && a.size() < 100000; p = U32(p + ib)) a.push_back(p);
        return a;
    }
};

struct MAPPING
{
    std::unordered_map<uint32_t, LO*> mpaplo;  // reference address -> native object
    std::unordered_map<const LO*, uint32_t> mpploa;
    int cmapped = 0, cunmapped = 0;
};


// (class, oid, creation order): the reference's class lists (SW +0x3038 + cid * 12) and the native ones
void MapObjects(SW* psw, const RAM& ram, MAPPING& map);
// SMs re-mapped by their transitions' ASEGs
void RemapSms(SW* psw, const RAM& ram, MAPPING& map);
