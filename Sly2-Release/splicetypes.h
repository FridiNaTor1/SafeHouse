#pragma once

// The small value types the splice runtime carries (Sly1-Release util.h's CLQ / LM / SMP, same layouts).
// When the engine's util.h arrives, these move there (keep the field names and order).

#include <cstdint>

using SYMID = uint32_t;

// A limit {gMin, gMax}.
struct LM
{
    float gMin;
    float gMax;

    constexpr LM(float min = 0, float max = 1) : gMin(min), gMax(max) {}
};

// A quadratic g0 + g1 x + g2 x^2.
struct CLQ
{
    float g0;
    float g1;
    float g2;
    float gUnused;
};

// Smoothing parameters.
struct SMP
{
    float svFast;
    float svSlow;
    float dtFast;
};

struct BASIC;
class SW;
