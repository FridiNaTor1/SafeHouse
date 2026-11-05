#pragma once

// The path curves (Sly 1's CRV / CRVL / CRVC; Sly 2's PATH objects, cid 0x0A, code 0x1E5D80..0x1E7D90): a linear or
// cubic Hermite curve through points with parameters (times) in the object's frame; distances along it in the
// world. Ported from the reference's m_1E5FC8_p3.cpp (oracle-checked ports of the original) and m_117030.cpp.

#include <glm/glm.hpp>

#include <vector>

struct LO;
struct LOXPATH;

struct CRV
{
    const LOXPATH* ppath = nullptr;
    int crvk = 0;                     // +0xE0 (0 linear, else cubic)
    int fClosed = 0;                  // +0xE4
    std::vector<float> ag;            // +0xEC the points' parameters
    std::vector<float> as;            // +0xF0 the points' distances (world)
    std::vector<glm::vec3> apos;      // +0xF4 (the object's frame)
    std::vector<glm::vec3> avIn, avOut;  // +0x140 / +0x144 (cubic)
    glm::mat4 mat{1.0f};              // +0x100 (the object's world matrix, cached)
    mutable int iseg = 0;             // +0x178 (the last segment found)
    mutable int isegCached = -1;      // +0x148
    mutable float asSub[11]{};        // +0x14C (the cached segment's 11 sub-lengths)
};

// orig 0x231440 (LOX vt slot 31): a located object's world matrix
glm::mat4 MatWorldLox(const LO* plo);
// The curve of a PATH object (built on first use and when its world matrix changed: 0x1E6180).
const CRV* PcrvFromLo(LO* plo);
// (the cached curve of a PATH object dropped: its points changed, e.g. a merged nav polygon reused, nav.cpp)
void InvalidateCrvOfLo(const LO* plo);
// orig 0x1E6180 (recomputes the distances for the matrix)
void RecalcCrv(CRV* pcrv, const glm::mat4& mat);

float SFromCrvG(const CRV* pcrv, float g);   // 0x1E63C0 the distance at parameter g
float GFromCrvS(const CRV* pcrv, float s);   // 0x1E64C8 the parameter at distance s
float GWrapCrv(const CRV* pcrv, float g);    // 0x1E6660
float SWrapCrv(const CRV* pcrv, float s);    // 0x1E66C8
float SLengthCrv(const CRV* pcrv);           // 0x1E6648
// orig 0x1E67B8: the world position / unit direction at parameter g (either may be null)
void EvalCrv(const CRV* pcrv, float g, glm::vec3* ppos, glm::vec3* pdir);
// orig 0x1E6F10: at distance s
void EvalCrvS(const CRV* pcrv, float s, glm::vec3* ppos, glm::vec3* pdir);
// orig 0x1E6ED8 (0x1E6A30 linear / 0x1E6DB8 cubic), searching from parameter g: the nearest point's position,
// direction, parameter, distance (each may be null)
void NearestCrv(const CRV* pcrv, const glm::vec3& q, float g, glm::vec3* ppos, glm::vec3* pdir, float* pg, float* ps,
                const glm::vec3* paxisFlat = nullptr);
// orig 0x1E7380: from distance s
void NearestCrvS(const CRV* pcrv, const glm::vec3& q, float s, glm::vec3* ppos, glm::vec3* pdir, float* pg, float* ps,
                 const glm::vec3* paxisFlat = nullptr);
// orig 0x1E6F60: over the whole curve
void NearestCrvAll(const CRV* pcrv, const glm::vec3& q, glm::vec3* ppos, glm::vec3* pdir, float* pg, float* ps,
                   const glm::vec3* paxisFlat = nullptr);
// (paxisFlat: the original's filter argument, a constraint of kind 1 (0x1FA970): the vectors' components along
// the axis dropped; the AI passes D_003DD020, the vertical: the nearest point in the horizontal plane)

// orig 0x11D9E0 MinimizeRange: the g of [gMin, gMax] minimising f, from g in steps dg, then 10 halvings
template <class F>
void MinimizeRange(F f, float g, float dg, float gMin, float gMax, float* pg, float* pgResult)
{
    struct GG { float g, r; } a[5];
    if (g < gMin) g = gMin;
    else if (gMax < g) g = gMax;
    a[0].g = gMin > g - dg ? gMin : g - dg;
    a[0].r = f(a[0].g);
    a[2].g = g;
    a[2].r = f(g);
    a[4].g = gMax < g + dg ? gMax : g + dg;
    a[4].r = f(a[4].g);
    for (int c = 0; c < 100000; ++c)
    {
        if (a[0].r < a[2].r)
        {
            a[4] = a[2];
            a[2] = a[0];
            const float x = (a[2].g + a[2].g) - a[4].g;
            a[0].g = gMin > x ? gMin : x;
            a[0].r = f(a[0].g);
        }
        else if (a[4].r < a[2].r)
        {
            a[0] = a[2];
            a[2] = a[4];
            const float x = (a[2].g + a[2].g) - a[0].g;
            a[4].g = gMax < x ? gMax : x;
            a[4].r = f(a[4].g);
        }
        else break;
    }
    for (int i = 9; i >= 0; --i)
    {
        a[1].g = (a[0].g + a[2].g) * 0.5f;
        a[1].r = f(a[1].g);
        a[3].g = (a[2].g + a[4].g) * 0.5f;
        a[3].r = f(a[3].g);
        if (a[1].r < a[2].r)
        {
            if (a[1].r < a[3].r)
            {
                a[4] = a[2];
                a[2] = a[1];
            }
            else
            {
                a[0] = a[2];
                a[2] = a[3];
            }
        }
        else if (a[2].r <= a[3].r)
        {
            a[0] = a[1];
            a[4] = a[3];
        }
        else
        {
            a[0] = a[2];
            a[2] = a[3];
        }
    }
    if (pg) *pg = a[2].g;
    if (pgResult) *pgResult = a[2].r;
}
