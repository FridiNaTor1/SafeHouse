// The map editor's models and textures (phase 4; notes/EDITOR_2026-11-12.md): the glob set stream (glob.cpp's
// LoadGlobsetFromBrx) parsed and written, a group's Graphics part (shd.cpp's LoadShadersFromBrx) parsed and extended,
// PNG -> the game's indexed textures, GLB / FBX import (editor/core/model.h) and GLB export. SLY2_EDITOR builds only.
#include "editor_impl.h"

#if SLY2_EDITOR

#include "animfind.h"
#include "aseg.h"
#include "asega.h"
#include "anim.h"
#include "bone.h"
#include "clock.h"
#include "xf.h"
#include "xfphys.h"
#include "so.h"
#include "texmod.h"
#include "core/model.h"

#include <nlohmann/json.hpp>

#include <imgui/ImGuiFileDialog.h>
#include <glad/glad.h>

#include <array>
#include <filesystem>
#include <cfloat>
#include <chrono>
#include <functional>
#include <fstream>
#include <numeric>
#include <set>

namespace edimpl
{
namespace md = cane::editor::model;
namespace fs = std::filesystem;

// ---- a little reader / writer of the stream's little-endian values

struct RD
{
    const uint8_t* pb = nullptr;
    std::size_t cb = 0, ib = 0;
    bool fBad = false;
    bool need(std::size_t c) { if (ib + c > cb) { fBad = true; ib = cb; return false; } return true; }
    template <class T> T get() { T v{}; if (need(sizeof(T))) { std::memcpy(&v, pb + ib, sizeof(T)); ib += sizeof(T); } return v; }
    void skip(std::size_t c) { if (need(c)) ib += c; }
    glm::vec3 vec() { glm::vec3 v; v.x = get<float>(); v.y = get<float>(); v.z = get<float>(); return v; }
};
struct WR
{
    std::vector<uint8_t> ab;
    template <class T> void put(T v) { const uint8_t* p = reinterpret_cast<const uint8_t*>(&v); ab.insert(ab.end(), p, p + sizeof(T)); }
    void vec(const glm::vec3& v) { put(v.x); put(v.y); put(v.z); }
    void bytes(const void* pv, std::size_t c) { const uint8_t* p = static_cast<const uint8_t*>(pv); ab.insert(ab.end(), p, p + c); }
};

// ---- the glob set stream (parse only: what the editor shows and copies; glob.cpp is the reader)

struct GLOBINFO
{
    std::size_t ib = 0;          // its S8 filter count in the glob set
    int cfilter = 0, meshk = 0, csub = 0, oidOwner = -1, rp = 0, n0 = 0, rtck = 0, fHidden = 0;
    float gZOrder = FLT_MAX, uFog = 0.0f, sRadius = 0.0f, sMRD = FLT_MAX;
    glm::vec3 posCenter{0.0f};
    uint32_t u14 = 0;
    int grfMesh = 0, isaa = -1, iSpec = -1, cbone = 0, cbatch = 0, cvtx = 0, ctri = 0;
    std::vector<std::pair<int, int>> abone;   // (oid, record)
    std::set<int> setishd;
};
struct GLOBSETINFO
{
    bool fNoDraw = false;
    glm::vec3 posCenter{0.0f};
    float sRadius = 0.0f;
    int csaa = 0;
    std::vector<int> aids;
    std::vector<GLOBINFO> aglob;
    int csphereset = 0;
};

namespace
{
void SkipIdxTab(RD& r)
{
    const int c = r.get<uint16_t>();
    for (int i = 0; i < c && !r.fBad; ++i) { const int n = r.get<uint16_t>(); r.skip(std::size_t(n) * 5); }
    if (c == 0) return;
    const int m = r.get<uint16_t>();
    r.skip(std::size_t(m) * 2);
}
void ReadBonesInfo(RD& r, GLOBINFO& g)
{
    g.cbone = int8_t(r.get<uint8_t>());
    for (int i = 0; i < std::max(0, g.cbone) && !r.fBad; ++i)
    {
        const int oid = r.get<int32_t>();
        const int ib = r.get<int16_t>();
        g.abone.push_back({oid, ib});
    }
}
void SkipInstances(RD& r, int csub, bool fDraw)
{
    for (int i = 0; i < csub && !r.fBad; ++i)
    {
        r.skip(1);
        if (fDraw) r.skip(4);
        r.skip(2 + 48 + 4 + 1);
        SkipIdxTab(r);
    }
}
}  // namespace

bool FParseGlobset(const uint8_t* pb, std::size_t cb, GLOBSETINFO& gi, std::string& error)
{
    RD r{pb, cb};
    gi = GLOBSETINFO();
    gi.fNoDraw = r.get<int8_t>() != 0;
    const bool fDraw = !gi.fNoDraw;
    gi.posCenter = r.vec();
    gi.sRadius = r.get<float>();
    gi.csaa = r.get<uint8_t>();
    for (int i = 0; i < gi.csaa; ++i) { const int cid = r.get<uint16_t>(); if (cid != 0xFFFF) r.skip(0x20); }
    const int cids = r.get<uint8_t>();
    for (int i = 0; i < cids; ++i) gi.aids.push_back(r.get<int32_t>());
    const int cglob = r.get<int16_t>();
    for (int i = 0; i < cglob && !r.fBad; ++i)
    {
        GLOBINFO g;
        g.ib = r.ib;
        g.cfilter = r.get<int8_t>();
        if (g.cfilter != 0)
        {
            r.skip(std::size_t(std::max(0, g.cfilter)) * 2);
            const int32_t cbSkip = r.get<int32_t>();
            r.skip(std::size_t(std::max(0, cbSkip)));
            g.meshk = -1;   // (filtered: not parsed)
            gi.aglob.push_back(g);
            continue;
        }
        g.meshk = r.get<int8_t>();
        g.csub = r.get<uint16_t>();
        g.oidOwner = r.get<int16_t>();
        g.rp = r.get<int8_t>();
        g.gZOrder = r.get<float>();
        g.uFog = r.get<float>();
        g.n0 = r.get<int32_t>();
        g.rtck = r.get<int8_t>();
        g.fHidden = r.get<int8_t>();
        if (fDraw)
        {
            g.posCenter = r.vec();
            g.sRadius = r.get<float>();
            g.u14 = r.get<uint32_t>();
            g.sMRD = r.get<float>();
        }
        switch (g.meshk)
        {
        case 0:
        {
            g.grfMesh = r.get<uint8_t>();
            g.isaa = r.get<int16_t>();
            ReadBonesInfo(r, g);
            g.iSpec = r.get<int8_t>();
            if (g.iSpec != -1) r.skip(12 + 8 + 12);
            SkipIdxTab(r);
            if (g.grfMesh & 4)
            {
                r.skip(4);
                if (r.get<int8_t>() != -1) r.skip(16 + 48);
                r.skip(3);
            }
            g.cbatch = r.get<int16_t>();
            for (int ib = 0; ib < g.cbatch && !r.fBad; ++ib)
            {
                const int c = r.get<uint8_t>();
                uint32_t N = 0;
                for (int e = 0; e < c; ++e)
                {
                    const uint32_t count = r.get<uint32_t>(), ishd = r.get<uint32_t>();
                    (void)r.get<uint32_t>();
                    N += count;
                    g.setishd.insert(int(ishd));
                    if (count >= 3) g.ctri += int(count) - 2;
                }
                const int M = r.get<uint8_t>();
                r.skip(std::size_t(M) * 36);
                r.skip(1);
                if (g.cbone != 0) { r.skip(4); r.skip(std::size_t(std::min(g.cbone, 4)) * std::size_t(M)); }
                r.skip(N);
                r.skip(((N + 31) / 32) * 4);
                g.cvtx += M;
            }
            break;
        }
        case 1: case 2: case 3:
        {
            ReadBonesInfo(r, g);
            const int cstrip = r.get<int16_t>();
            for (int s = 0; s < cstrip && !r.fBad; ++s)
            {
                if (g.meshk != 1) g.setishd.insert(r.get<uint16_t>());
                const int cv = r.get<uint8_t>();
                if (g.meshk == 3)
                {
                    const int cpos = r.get<uint8_t>();
                    r.skip(std::size_t(cpos) * 12);
                    if (g.cbone != 0) r.skip(4 + std::size_t(std::min(g.cbone, 4)) * std::size_t(cpos));
                    r.skip(std::size_t(cv));
                    r.skip(std::size_t((cv + 31) / 32) * 4);
                }
                else
                {
                    r.skip(std::size_t(cv) * 12);
                    if (g.cbone != 0) { const int cw = r.get<uint8_t>(); r.skip(std::size_t(cw)); r.skip(std::size_t(cv) * std::size_t(cw)); }
                    const int cn = r.get<uint8_t>();
                    r.skip(std::size_t(cn) * 4);
                }
                g.cvtx += cv;
            }
            break;
        }
        default: error = "unknown mesh type " + std::to_string(g.meshk); return false;
        }
        SkipInstances(r, g.csub, fDraw);
        i += g.csub;
        gi.aglob.push_back(std::move(g));
    }
    const int cset = r.get<int16_t>();
    gi.csphereset = cset;
    for (int s = 0; s < cset && !r.fBad; ++s)
        for (int k = 0; k < 4; ++k)
        {
            const int8_t v = r.get<int8_t>();
            const int kind = int8_t(v << 4) >> 4;
            if (kind == -1) break;
            r.skip(12 + 4 + 4 + 1);
            if (kind == 0 || kind == 1) r.skip(2);
        }
    if (r.fBad) { error = "the glob set ends early"; return false; }
    if (r.ib != cb) { error = "the glob set has " + std::to_string(cb - r.ib) + " bytes left over"; return false; }
    return true;
}


// ---- an ALO's collision entries (alo.cpp LoadAloFromBrx: S16 count, the SOEs, then mass + inertia when any)

struct SOEINFO
{
    int ixf = 0, tag = 0, f48 = 0, f49 = 0;
    float sRadius = 0.0f;
    int cpos = 0, csurf = 0, cedge = 0;
    uint32_t cbsp = 0, x4 = 0, x8 = 0;
    std::vector<std::array<uint32_t, 3>> abspn;   // isurf, neg, pos
    uint32_t id = 0;
    int state = 0, pinned = 0, f50 = 0;
    glm::vec3 posMin{FLT_MAX}, posMax{-FLT_MAX};
};
struct SOESINFO
{
    std::vector<SOEINFO> asoe;
    float gMass = 0.0f;
    float agInertia[9] = {};
};

bool FParseSoes(const uint8_t* pb, std::size_t cb, SOESINFO& si, std::string& error)
{
    RD r{pb, cb};
    si = SOESINFO();
    const int c = r.get<int16_t>();
    for (int i = 0; i < c && !r.fBad; ++i)
    {
        SOEINFO e;
        e.ixf = r.get<int16_t>();
        e.tag = r.get<int32_t>();
        e.f48 = r.get<int8_t>();
        e.f49 = r.get<int8_t>();
        e.sRadius = r.get<float>();
        e.cpos = r.get<uint16_t>();
        for (int k = 0; k < e.cpos; ++k) { const glm::vec3 p = r.vec(); e.posMin = glm::min(e.posMin, p); e.posMax = glm::max(e.posMax, p); }
        e.csurf = r.get<uint16_t>();
        e.cedge = r.get<uint16_t>();
        for (int k = 0; k < e.csurf && !r.fBad; ++k)
        {
            r.skip(6 + 16);
            const int ce = r.get<uint8_t>();
            r.skip(std::size_t(ce) * 6);
        }
        e.cbsp = r.get<uint32_t>();
        e.x4 = r.get<uint32_t>();
        e.x8 = r.get<uint32_t>();
        for (uint32_t k = 0; k < e.cbsp && !r.fBad; ++k)
        {
            std::array<uint32_t, 3> n{r.get<uint16_t>(), 0xFFFFFFFFu, 0xFFFFFFFFu};
            if (n[0] != 0xFFFF) { n[1] = r.get<uint32_t>(); n[2] = r.get<uint32_t>(); }
            e.abspn.push_back(n);
        }
        e.id = r.get<uint32_t>();
        e.state = r.get<int16_t>();
        e.pinned = r.get<int16_t>();
        e.f50 = r.get<int8_t>();
        si.asoe.push_back(std::move(e));
    }
    if (c > 0)
    {
        si.gMass = r.get<float>();
        for (float& g : si.agInertia) g = r.get<float>();
    }
    if (r.fBad || r.ib != cb) { error = "the collision entries do not parse"; return false; }
    return true;
}

// A convex box as one collision entry (the fields of a model entry, e.g. the replaced object's first): 8 corners, two
// triangles a face (each with its three edges and the face it borders), and a BSP chain of one triangle per face
// (behind every face: inside; bsp.cpp IbspFindPoint).
void WriteBoxSoe(WR& w, const SOEINFO& tmpl, const glm::vec3& mn, const glm::vec3& mx)
{
    w.put<int16_t>(int16_t(tmpl.ixf));
    w.put<int32_t>(tmpl.tag);
    w.put<int8_t>(int8_t(tmpl.f48));
    w.put<int8_t>(int8_t(tmpl.f49));
    w.put<float>(glm::length(mx - mn) * 0.5f);
    glm::vec3 apos[8];
    for (int i = 0; i < 8; ++i) apos[i] = glm::vec3(i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z);
    w.put<uint16_t>(8);
    for (const glm::vec3& p : apos) w.vec(p);
    // faces as quads (counter-clockwise seen from outside: the plane's normal points out)
    static const int s_aface[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    int atri[12][3];
    for (int f = 0; f < 6; ++f)
    {
        const int* q = s_aface[f];
        atri[2 * f][0] = q[0]; atri[2 * f][1] = q[1]; atri[2 * f][2] = q[2];
        atri[2 * f + 1][0] = q[0]; atri[2 * f + 1][1] = q[2]; atri[2 * f + 1][2] = q[3];
    }
    auto other = [&](int t, int a, int b) {
        for (int u = 0; u < 12; ++u)
        {
            if (u == t) continue;
            for (int k = 0; k < 3; ++k)
                if (atri[u][k] == b && atri[u][(k + 1) % 3] == a) return u;
        }
        return 0xFFFF;
    };
    w.put<uint16_t>(12);
    w.put<uint16_t>(36);
    for (int t = 0; t < 12; ++t)
    {
        for (int k = 0; k < 3; ++k) w.put<uint16_t>(uint16_t(atri[t][k]));
        for (int k = 0; k < 4; ++k) w.put<float>(0.0f);
        w.put<uint8_t>(3);
        for (int k = 0; k < 3; ++k)
        {
            const int a = atri[t][k], b = atri[t][(k + 1) % 3];
            w.put<uint16_t>(uint16_t(a));
            w.put<uint16_t>(uint16_t(b));
            w.put<uint16_t>(uint16_t(other(t, a, b)));
        }
    }
    w.put<uint32_t>(6);
    w.put<uint32_t>(tmpl.x4);
    w.put<uint32_t>(tmpl.x8);
    for (uint32_t f = 0; f < 6; ++f)
    {
        w.put<uint16_t>(uint16_t(2 * f));
        w.put<uint32_t>(f + 1 < 6 ? f + 1 : 0xFFFFFFFFu);
        w.put<uint32_t>(0xFFFFFFFFu);
    }
    w.put<uint32_t>(tmpl.id);
    w.put<int16_t>(int16_t(tmpl.state));
    w.put<int16_t>(int16_t(tmpl.pinned));
    w.put<int8_t>(int8_t(tmpl.f50));
}

// A convex piece as one collision entry (as WriteBoxSoe: its faces' triangles with their edges and neighbours, a BSP
// chain of one triangle per face: behind every face is inside)
void WriteConvexSoe(WR& w, const SOEINFO& tmpl, const CONVEX& cv)
{
    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
    for (const glm::vec3& p : cv.apos) { mn = glm::min(mn, p); mx = glm::max(mx, p); }
    w.put<int16_t>(int16_t(tmpl.ixf));
    w.put<int32_t>(tmpl.tag);
    w.put<int8_t>(int8_t(tmpl.f48));
    w.put<int8_t>(int8_t(tmpl.f49));
    w.put<float>(glm::length(mx - mn) * 0.5f);
    w.put<uint16_t>(uint16_t(cv.apos.size()));
    for (const glm::vec3& p : cv.apos) w.vec(p);
    std::vector<std::array<int, 3>> atri;
    std::vector<int> aitriFirst;
    for (const std::vector<int>& f : cv.aface)
    {
        aitriFirst.push_back(int(atri.size()));
        for (std::size_t k = 1; k + 1 < f.size(); ++k) atri.push_back({f[0], f[k], f[k + 1]});
    }
    auto other = [&](int t, int a, int b) {
        for (int u = 0; u < int(atri.size()); ++u)
        {
            if (u == t) continue;
            for (int k = 0; k < 3; ++k)
                if (atri[std::size_t(u)][std::size_t(k)] == b && atri[std::size_t(u)][std::size_t((k + 1) % 3)] == a) return u;
        }
        return 0xFFFF;
    };
    w.put<uint16_t>(uint16_t(atri.size()));
    w.put<uint16_t>(uint16_t(atri.size() * 3));
    for (int t = 0; t < int(atri.size()); ++t)
    {
        for (int k = 0; k < 3; ++k) w.put<uint16_t>(uint16_t(atri[std::size_t(t)][std::size_t(k)]));
        for (int k = 0; k < 4; ++k) w.put<float>(0.0f);
        w.put<uint8_t>(3);
        for (int k = 0; k < 3; ++k)
        {
            const int a = atri[std::size_t(t)][std::size_t(k)], b = atri[std::size_t(t)][std::size_t((k + 1) % 3)];
            w.put<uint16_t>(uint16_t(a));
            w.put<uint16_t>(uint16_t(b));
            w.put<uint16_t>(uint16_t(other(t, a, b)));
        }
    }
    const uint32_t cnode = uint32_t(cv.aface.size());
    w.put<uint32_t>(cnode);
    w.put<uint32_t>(cnode);
    w.put<uint32_t>(tmpl.x8);
    for (uint32_t f = 0; f < cnode; ++f)
    {
        w.put<uint16_t>(uint16_t(aitriFirst[f]));
        w.put<uint32_t>(f + 1 < cnode ? f + 1 : 0xFFFFFFFFu);
        w.put<uint32_t>(0xFFFFFFFFu);
    }
    w.put<uint32_t>(tmpl.id);
    w.put<int16_t>(int16_t(tmpl.state));
    w.put<int16_t>(int16_t(cv.apos.size()));
    w.put<int8_t>(int8_t(tmpl.f50));
}

// ---- a group's Graphics part (shd.cpp LoadShadersFromBrx): CLUTs, BMPs, SAAs, shaders, Align(16), the bulk

struct GFXCLUT { std::size_t ib = 0, ibData = 0; int id = 0, texk = 0, ccolor = 0; uint32_t ibBulk = 0; };
struct GFXBMP { std::size_t ib = 0, ibData = 0; int id = 0, texk = 0, dx = 0, dy = 0, psm = 0; uint32_t cb = 0, ibBulk = 0; };
struct GFXTEX { int oid = 0, grftex = 0; std::vector<int> aibmp, aiclut; std::size_t ib = 0; };
struct GFXSHD { std::size_t ib = 0; int id = 0, shdk = 0, grfshd = 0; uint32_t argba[3] = {}; int isaa = -1; std::vector<GFXTEX> atex; };
struct GFXINFO
{
    std::vector<GFXCLUT> aclut;
    std::vector<GFXBMP> abmp;
    std::vector<GFXSHD> ashd;
    std::size_t ibCbmp = 0, ibCsaa = 0, ibCshd = 0, ibEndShd = 0;   // the U16 counts / the end of the shaders
};

bool FParseGraphics(const std::vector<uint8_t>& ab, GFXINFO& gi, std::string& error)
{
    RD r{ab.data(), ab.size()};
    gi = GFXINFO();
    const int cclut = r.get<uint16_t>();
    for (int i = 0; i < cclut && !r.fBad; ++i)
    {
        GFXCLUT c;
        c.ib = r.ib;
        c.id = r.get<int32_t>();
        c.texk = r.get<uint8_t>();
        const int cfilter = r.get<int8_t>();
        r.skip(2);
        c.ccolor = r.get<uint16_t>();
        r.skip(2);
        c.ibBulk = r.get<uint32_t>();
        if (cfilter > 0) r.skip(std::size_t(cfilter) * 2);
        c.ibData = r.ib;
        if (c.texk != TEXK_Bulk) r.skip(std::size_t(c.ccolor) * 4);
        gi.aclut.push_back(c);
    }
    gi.ibCbmp = r.ib;
    const int cbmp = r.get<uint16_t>();
    for (int i = 0; i < cbmp && !r.fBad; ++i)
    {
        GFXBMP b;
        b.ib = r.ib;
        b.id = r.get<int32_t>();
        b.texk = r.get<uint8_t>();
        const int cfilter = r.get<int8_t>();
        r.skip(2);
        b.dx = r.get<uint16_t>();
        b.dy = r.get<uint16_t>();
        b.psm = r.get<uint8_t>();
        r.skip(1 + 2);
        b.cb = r.get<uint32_t>();
        b.ibBulk = r.get<uint32_t>();
        if (cfilter > 0) r.skip(std::size_t(cfilter) * 2);
        b.ibData = r.ib;
        if (b.texk != TEXK_Bulk) r.skip(b.cb);
        gi.abmp.push_back(b);
    }
    gi.ibCsaa = r.ib;
    const int csaa = r.get<uint16_t>();
    for (int i = 0; i < csaa && !r.fBad; ++i) { const int cid = r.get<uint16_t>(); if (cid != 0xFFFF) r.skip(0x20); }
    gi.ibCshd = r.ib;
    const int cshd = r.get<uint16_t>();
    for (int i = 0; i < cshd && !r.fBad; ++i)
    {
        GFXSHD s;
        s.ib = r.ib;
        s.id = r.get<int32_t>();
        s.shdk = r.get<uint8_t>();
        s.grfshd = r.get<uint8_t>();
        r.skip(2);
        for (uint32_t& c : s.argba) c = r.get<uint32_t>();
        r.skip(3);
        const int ctex = r.get<uint8_t>();
        s.isaa = r.get<int16_t>();
        for (int t = 0; t < ctex && !r.fBad; ++t)
        {
            GFXTEX tx;
            tx.ib = r.ib;
            tx.oid = r.get<int32_t>();
            tx.grftex = r.get<uint16_t>();
            const int cb = r.get<uint8_t>(), cc = r.get<uint8_t>();
            for (int k = 0; k < cb; ++k) tx.aibmp.push_back(r.get<uint16_t>());
            for (int k = 0; k < cc; ++k) tx.aiclut.push_back(r.get<uint16_t>());
            s.atex.push_back(std::move(tx));
        }
        gi.ashd.push_back(std::move(s));
    }
    gi.ibEndShd = r.ib;
    if (r.fBad) { error = "the Graphics part ends early"; return false; }
    return true;
}

// ---- textures: PNG -> the game's indexed format

namespace
{
// The image resampled (bilinear on premultiplied colour) to w x h
md::Image Resample(const md::Image& src, int w, int h)
{
    md::Image out;
    out.w = w; out.h = h; out.name = src.name;
    out.rgba.resize(std::size_t(w) * std::size_t(h) * 4);
    if (src.w == w && src.h == h) { out.rgba = src.rgba; return out; }
    // box filter when shrinking (the average of the source pixels each covers), bilinear when growing
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const float x0 = float(x) * src.w / w, x1 = float(x + 1) * src.w / w;
            const float y0 = float(y) * src.h / h, y1 = float(y + 1) * src.h / h;
            double acc[4] = {}, wsum = 0;
            if (x1 - x0 <= 1.0f || y1 - y0 <= 1.0f)
            {
                const float fx = std::clamp((x + 0.5f) * src.w / w - 0.5f, 0.0f, float(src.w - 1));
                const float fy = std::clamp((y + 0.5f) * src.h / h - 0.5f, 0.0f, float(src.h - 1));
                const int ix = int(fx), iy = int(fy);
                const int ix1 = std::min(ix + 1, src.w - 1), iy1 = std::min(iy + 1, src.h - 1);
                const float ax = fx - ix, ay = fy - iy;
                const int sx[4] = {ix, ix1, ix, ix1}, sy[4] = {iy, iy, iy1, iy1};
                const float sw[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
                for (int k = 0; k < 4; ++k)
                {
                    const uint8_t* p = &src.rgba[(std::size_t(sy[k]) * src.w + sx[k]) * 4];
                    const double a = p[3] / 255.0 * sw[k];
                    for (int c = 0; c < 3; ++c) acc[c] += p[c] * a;
                    acc[3] += a;
                    wsum += sw[k];
                }
            }
            else
                for (int sy = int(y0); sy < int(std::ceil(y1)) && sy < src.h; ++sy)
                    for (int sx = int(x0); sx < int(std::ceil(x1)) && sx < src.w; ++sx)
                    {
                        const uint8_t* p = &src.rgba[(std::size_t(sy) * src.w + sx) * 4];
                        const double a = p[3] / 255.0;
                        for (int c = 0; c < 3; ++c) acc[c] += p[c] * a;
                        acc[3] += a;
                        wsum += 1;
                    }
            uint8_t* d = &out.rgba[(std::size_t(y) * w + x) * 4];
            for (int c = 0; c < 3; ++c) d[c] = uint8_t(acc[3] > 0 ? std::clamp(acc[c] / acc[3], 0.0, 255.0) + 0.5 : 0);
            d[3] = uint8_t(wsum > 0 ? std::clamp(acc[3] / wsum * 255.0 + 0.5, 0.0, 255.0) : 0);
        }
    return out;
}

// Median cut over RGBA: up to ccolor colours; aidx: each pixel's colour
void Quantize(const md::Image& img, int ccolor, std::vector<uint32_t>& apal, std::vector<uint8_t>& aidx)
{
    const std::size_t c = std::size_t(img.w) * std::size_t(img.h);
    std::vector<uint32_t> apx(c);
    for (std::size_t i = 0; i < c; ++i) std::memcpy(&apx[i], &img.rgba[i * 4], 4);
    // few colours: exact
    std::vector<uint32_t> auniq(apx);
    std::sort(auniq.begin(), auniq.end());
    auniq.erase(std::unique(auniq.begin(), auniq.end()), auniq.end());
    if (int(auniq.size()) <= ccolor) apal = auniq;
    else
    {
        struct BOX { std::vector<uint32_t> a; };
        std::vector<BOX> abox(1);
        abox[0].a = apx;
        auto range = [](const BOX& b, int& ch) {
            int best = -1;
            for (int k = 0; k < 4; ++k)
            {
                int mn = 255, mx = 0;
                for (uint32_t p : b.a) { const int v = (p >> (8 * k)) & 0xFF; mn = std::min(mn, v); mx = std::max(mx, v); }
                if (mx - mn > best) { best = mx - mn; ch = k; }
            }
            return best;
        };
        while (int(abox.size()) < ccolor)
        {
            int ibest = -1, rbest = 0, chbest = 0;
            for (std::size_t i = 0; i < abox.size(); ++i)
            {
                if (abox[i].a.size() < 2) continue;
                int ch = 0;
                const int rg = range(abox[i], ch);
                if (rg > rbest) { rbest = rg; ibest = int(i); chbest = ch; }
            }
            if (ibest < 0) break;
            std::vector<uint32_t>& a = abox[size_t(ibest)].a;
            std::sort(a.begin(), a.end(), [chbest](uint32_t p, uint32_t q) { return ((p >> (8 * chbest)) & 0xFF) < ((q >> (8 * chbest)) & 0xFF); });
            BOX b2;
            b2.a.assign(a.begin() + std::ptrdiff_t(a.size() / 2), a.end());
            a.resize(a.size() / 2);
            abox.push_back(std::move(b2));
        }
        apal.clear();
        for (const BOX& b : abox)
        {
            if (b.a.empty()) continue;
            uint64_t s[4] = {};
            for (uint32_t p : b.a)
                for (int k = 0; k < 4; ++k) s[k] += (p >> (8 * k)) & 0xFF;
            uint32_t col = 0;
            for (int k = 0; k < 4; ++k) col |= uint32_t((s[k] + b.a.size() / 2) / b.a.size()) << (8 * k);
            apal.push_back(col);
        }
    }
    aidx.resize(c);
    std::unordered_map<uint32_t, uint8_t> mpnear;
    for (std::size_t i = 0; i < c; ++i)
    {
        const uint32_t p = apx[i];
        if (auto it = mpnear.find(p); it != mpnear.end()) { aidx[i] = it->second; continue; }
        int best = 0;
        long dbest = LONG_MAX;
        for (std::size_t j = 0; j < apal.size(); ++j)
        {
            long d = 0;
            for (int k = 0; k < 4; ++k) { const long e = long((p >> (8 * k)) & 0xFF) - long((apal[j] >> (8 * k)) & 0xFF); d += e * e; }
            if (d < dbest) { dbest = d; best = int(j); }
        }
        mpnear[p] = uint8_t(best);
        aidx[i] = uint8_t(best);
    }
}

int Pow2Below(int n, int nMax)
{
    int p = 1;
    while (p * 2 <= n && p * 2 <= nMax) p *= 2;
    if (p < n && p * 2 <= nMax && (n - p) > (p * 2 - n)) p *= 2;   // (nearer the next one up)
    return std::max(p, 1);
}
}  // namespace

// An image as the game's texels: a CLUT (in upload order) and T8 / T4 pixels, rows bottom-up (the PNG's last row
// first), alpha in the PS2's range. dx / dy: powers of two up to nMax.
struct GAMETEX
{
    int dx = 0, dy = 0;
    bool fT4 = false;
    std::vector<uint32_t> aclut;   // 256 (T8) or 16 (T4) entries, as stored
    std::vector<uint8_t> apix;     // as stored
    // the same colours as greys (each channel the mean of the three): a lit (kind 1) shader's first pass draws the
    // bitmap with it under the ambient colour, the second with the colours under the light (as the game's own models)
    std::vector<uint32_t> GreyClut() const
    {
        std::vector<uint32_t> a(aclut);
        for (uint32_t& c : a)
        {
            const uint32_t g = (((c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF)) + 1) / 3;
            c = (c & 0xFF000000u) | g | (g << 8) | (g << 16);
        }
        return a;
    }
};
md::Image GreyImage(md::Image img)
{
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4)
    {
        const uint8_t g = uint8_t((img.rgba[i] + img.rgba[i + 1] + img.rgba[i + 2] + 1) / 3);
        img.rgba[i] = img.rgba[i + 1] = img.rgba[i + 2] = g;
    }
    return img;
}
GAMETEX GametexFromImage(const md::Image& src, int nMax, int dxWant = 0, int dyWant = 0, int ccolorWant = 0)
{
    GAMETEX t;
    t.dx = dxWant ? dxWant : Pow2Below(src.w, nMax);
    t.dy = dyWant ? dyWant : Pow2Below(src.h, nMax);
    const md::Image img = Resample(src, t.dx, t.dy);
    md::Image flipped = img;
    for (int y = 0; y < t.dy; ++y)
        for (int x = 0; x < t.dx; ++x)
            for (int k = 0; k < 4; ++k)
            {
                const uint8_t v = img.rgba[(std::size_t(t.dy - 1 - y) * t.dx + x) * 4 + k];
                flipped.rgba[(std::size_t(y) * t.dx + x) * 4 + k] = k < 3 ? v : uint8_t((v + 1) / 2);
            }
    std::vector<uint32_t> apal;
    std::vector<uint8_t> aidx;
    const int ccolor = ccolorWant ? ccolorWant : 256;
    Quantize(flipped, ccolor, apal, aidx);
    t.fT4 = ccolor <= 16;
    apal.resize(t.fT4 ? 16 : 256, 0);
    t.aclut.resize(apal.size());
    if (t.fT4) t.aclut = apal;
    else
        for (int i = 0; i < 256; ++i)
        {
            const int s = (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1);   // (CSM1: bits 3 and 4 exchanged)
            t.aclut[size_t(s)] = apal[size_t(i)];
        }
    const std::size_t c = std::size_t(t.dx) * std::size_t(t.dy);
    if (t.fT4)
    {
        t.apix.assign((c + 1) / 2, 0);
        for (std::size_t i = 0; i < c; ++i) t.apix[i / 2] |= uint8_t((aidx[i] & 15) << ((i & 1) * 4));
    }
    else t.apix = aidx;
    return t;
}

// The texels as the renderer decodes them (shd.cpp FDecodeTexture) and their texture-pack name (texmod.h)
std::string SzModNameOf(const GAMETEX& t, std::vector<uint8_t>* prgba = nullptr, bool fGrey = false)
{
    BMP bmp;
    bmp.dx = uint16_t(t.dx); bmp.dy = uint16_t(t.dy);
    bmp.psm = t.fT4 ? 0x14 : 0x13;
    bmp.cb = uint32_t(t.apix.size());
    bmp.pab = std::make_shared<std::vector<uint8_t>>(t.apix);
    CLUT clut;
    clut.ccolor = uint16_t(t.aclut.size());
    clut.pargba = std::make_shared<std::vector<uint32_t>>(fGrey ? t.GreyClut() : t.aclut);
    std::vector<uint8_t> rgba;
    std::string err;
    if (!FDecodeTexture(&bmp, &clut, rgba, err)) return std::string();
    const std::string name = SzTextureModName(rgba, t.dx, t.dy);
    if (prgba) *prgba = std::move(rgba);
    return name;
}

// A decoded texture (FDecodeTexture's rows) as a PNG-orientation image (top-down, alpha doubled)
md::Image ImageFromDecoded(const std::vector<uint8_t>& rgba, int dx, int dy, const std::string& name)
{
    md::Image img;
    img.name = name; img.w = dx; img.h = dy;
    img.rgba.resize(rgba.size());
    for (int y = 0; y < dy; ++y)
        for (int x = 0; x < dx; ++x)
            for (int k = 0; k < 4; ++k)
            {
                const uint8_t v = rgba[(std::size_t(y) * dx + x) * 4 + k];
                img.rgba[(std::size_t(dy - 1 - y) * dx + x) * 4 + k] = k < 3 ? v : uint8_t(std::min(255, v * 2));
            }
    return img;
}



// ---- the glob set written (the reader's format, glob.cpp): one type-0 glob per material, its triangles as 3-vertex
// strips (the first two vertices of each with the ADC bit, odd triangles' first two exchanged: BuildTriangles flips
// them back), batches of up to 255 pool vertices

struct VTXOUT
{
    glm::vec3 pos{0.0f}, nrm{0.0f, 0.0f, 1.0f};
    glm::vec2 uv{0.0f};          // the game's (v up)
    uint32_t rgba = 0xFFFFFFFFu;
    uint8_t aj[4] = {}, aw[4] = {};   // skinned: up to four of the glob's bones and their weights (/255)
    bool operator==(const VTXOUT& o) const
    {
        return pos == o.pos && nrm == o.nrm && uv == o.uv && rgba == o.rgba && !std::memcmp(aj, o.aj, 4) && !std::memcmp(aw, o.aw, 4);
    }
};
struct GLOBOUT
{
    int ishd = 0;
    bool fBlend = false;
    std::vector<VTXOUT> avtx;            // three per triangle
    std::vector<std::pair<int, int>> abone;   // skinned: (oid, record) of the glob's bones
    int oidOwner = INT_MIN, n0 = INT_MIN, rp = -1;   // (INT_MIN / -1: the header's)
    bool fHidden = false;
    bool fOutline = true;                // with a cel border (a type-1 glob of its silhouette edges)
};
struct GLOBHDROUT   // the header fields taken from the replaced model (or defaults)
{
    int oidOwner = -1, rp = 3, n0 = 0, rtck = -1, grfEntry = 3;
    uint32_t u14 = 2;
    float uFog = FLT_MAX;
};

namespace
{
void Bounds(const std::vector<VTXOUT>& a, glm::vec3& c, float& r)
{
    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
    for (const VTXOUT& v : a) { mn = glm::min(mn, v.pos); mx = glm::max(mx, v.pos); }
    if (a.empty()) { c = glm::vec3(0.0f); r = 0.0f; return; }
    c = (mn + mx) * 0.5f;
    r = 0.0f;
    for (const VTXOUT& v : a) r = std::max(r, glm::length(v.pos - c));
}
}  // namespace

std::vector<uint8_t> GlobsetBytes(const std::vector<GLOBOUT>& aglob, const GLOBHDROUT& hdr)
{
    WR w;
    std::vector<VTXOUT> aall;
    for (const GLOBOUT& g : aglob) aall.insert(aall.end(), g.avtx.begin(), g.avtx.end());
    glm::vec3 c;
    float r;
    Bounds(aall, c, r);
    w.put<int8_t>(0);
    w.vec(c);
    w.put<float>(r);
    w.put<uint8_t>(0);   // shader animations
    w.put<uint8_t>(0);   // ids
    int cout = 0;
    for (const GLOBOUT& g : aglob) cout += g.fOutline ? 2 : 1;
    w.put<int16_t>(int16_t(cout));
    for (const GLOBOUT& g : aglob)
    {
        glm::vec3 cg;
        float rg;
        Bounds(g.avtx, cg, rg);
        auto header = [&](int meshk) {
            w.put<int8_t>(0);                    // filters
            w.put<int8_t>(int8_t(meshk));
            w.put<uint16_t>(0);                  // instances
            w.put<int16_t>(int16_t(g.oidOwner != INT_MIN ? g.oidOwner : hdr.oidOwner));
            w.put<int8_t>(int8_t(meshk == 1 ? 9 : g.rp >= 0 ? g.rp : hdr.rp));
            w.put<float>(FLT_MAX);               // no z order
            w.put<float>(hdr.uFog);
            w.put<int32_t>(g.n0 != INT_MIN ? g.n0 : hdr.n0);
            w.put<int8_t>(int8_t(hdr.rtck));
            w.put<int8_t>(g.fHidden ? 1 : 0);
            w.vec(cg);
            w.put<float>(rg);
            w.put<uint32_t>(hdr.u14);
            w.put<float>(meshk == 1 ? 20000.0f : FLT_MAX);   // (cel borders fade out at 20000, as the characters')
        };
        header(0);
        w.put<uint8_t>(0);                   // grfMesh
        w.put<int16_t>(-1);                  // no shader animation
        w.put<uint8_t>(uint8_t(g.abone.size()));
        for (const auto& [oid, ixf] : g.abone) { w.put<int32_t>(oid); w.put<int16_t>(int16_t(ixf)); }
        w.put<int8_t>(-1);                   // no view-angle block
        w.put<uint16_t>(0);                  // no instance colour table
        // batches: the pool (deduplicated vertices, at most 255) and its triangles
        struct BATCH { std::vector<VTXOUT> apool; std::vector<uint8_t> aisrc; };
        std::vector<BATCH> abatch(1);
        for (std::size_t t = 0; t + 2 < g.avtx.size(); t += 3)
        {
            uint8_t ai[3];
            for (int pass = 0; pass < 2; ++pass)
            {
                BATCH& b = abatch.back();
                std::vector<int> aiNew;
                int cnew = 0;
                for (int k = 0; k < 3; ++k)
                {
                    const VTXOUT& v = g.avtx[t + size_t(k)];
                    int ip = -1;
                    for (std::size_t q = 0; q < b.apool.size(); ++q)
                        if (b.apool[q] == v) { ip = int(q); break; }
                    if (ip < 0)
                    {
                        // (a vertex repeated inside this triangle counts once)
                        for (int k2 = 0; k2 < k; ++k2)
                            if (g.avtx[t + size_t(k2)] == v && aiNew[size_t(k2)] >= int(b.apool.size())) ip = aiNew[size_t(k2)];
                        if (ip < 0) ip = int(b.apool.size()) + cnew++;
                    }
                    aiNew.push_back(ip);
                }
                if (b.apool.size() + size_t(cnew) > 255) { abatch.emplace_back(); continue; }
                for (int k = 0; k < 3; ++k)
                {
                    if (aiNew[size_t(k)] >= int(b.apool.size())) b.apool.push_back(g.avtx[t + size_t(k)]);
                    ai[k] = uint8_t(aiNew[size_t(k)]);
                }
                const bool fOdd = (b.aisrc.size() / 3) & 1;
                if (fOdd) { b.aisrc.push_back(ai[1]); b.aisrc.push_back(ai[0]); }
                else { b.aisrc.push_back(ai[0]); b.aisrc.push_back(ai[1]); }
                b.aisrc.push_back(ai[2]);
                break;
            }
        }
        if (abatch.back().aisrc.empty()) abatch.pop_back();
        w.put<int16_t>(int16_t(abatch.size()));
        for (const BATCH& b : abatch)
        {
            const uint32_t N = uint32_t(b.aisrc.size());
            w.put<uint8_t>(1);
            w.put<uint32_t>(N);
            w.put<uint32_t>(uint32_t(g.ishd));
            w.put<uint32_t>(uint32_t(hdr.grfEntry));
            w.put<uint8_t>(uint8_t(b.apool.size()));
            for (const VTXOUT& v : b.apool) w.vec(v.pos);
            for (const VTXOUT& v : b.apool) { w.put<float>(v.uv.x); w.put<float>(v.uv.y); }
            for (const VTXOUT& v : b.apool) w.vec(v.nrm);
            for (const VTXOUT& v : b.apool) w.put<uint32_t>(v.rgba);
            w.put<int8_t>(g.fBlend ? 1 : 0);
            if (!g.abone.empty())
            {
                // the batch's four bone slots: the glob's first four bones (the importer splits globs by bone sets)
                for (int k = 0; k < 4; ++k) w.put<uint8_t>(uint8_t(std::min<int>(k, int(g.abone.size()) - 1)));
                const int cw = std::min<int>(int(g.abone.size()), 4);
                for (const VTXOUT& v : b.apool)
                    for (int k = 0; k < cw; ++k) w.put<uint8_t>(v.aw[k]);
            }
            w.bytes(b.aisrc.data(), N);
            std::vector<uint8_t> amask(((N + 31) / 32) * 4, 0);
            for (uint32_t k = 0; k < N; ++k)
                if (k % 3 != 2) amask[k / 8] |= uint8_t(1u << (k % 8));
            w.bytes(amask.data(), amask.size());
        }
        if (!g.fOutline) continue;
        // the cel border: positions welded, each edge two triangles share as {OA, E0, E1, OB} (render.cpp: the
        // triangles (OA, E0, E1) and (E1, E0, OB)), in strips of up to 255 positions and 255 edges
        header(1);
        w.put<uint8_t>(uint8_t(g.abone.size()));
        for (const auto& [oid, ixf] : g.abone) { w.put<int32_t>(oid); w.put<int16_t>(int16_t(ixf)); }
        std::vector<int> aiweld(g.avtx.size());
        std::vector<int> aifirst;   // the welded positions' first vertex
        {
            std::map<std::array<float, 3>, int> mp;
            for (std::size_t i = 0; i < g.avtx.size(); ++i)
            {
                const std::array<float, 3> k{g.avtx[i].pos.x, g.avtx[i].pos.y, g.avtx[i].pos.z};
                const auto it = mp.emplace(k, int(aifirst.size()));
                if (it.second) aifirst.push_back(int(i));
                aiweld[i] = it.first->second;
            }
        }
        std::map<std::pair<int, int>, std::vector<std::array<int, 3>>> mpedge;   // (min, max) -> (e0, e1, opposite)
        for (std::size_t t = 0; t + 2 < g.avtx.size(); t += 3)
        {
            const int a[3] = {aiweld[t], aiweld[t + 1], aiweld[t + 2]};
            if (a[0] == a[1] || a[1] == a[2] || a[0] == a[2]) continue;
            for (int k = 0; k < 3; ++k)
            {
                const int e0 = a[k], e1 = a[(k + 1) % 3], o = a[(k + 2) % 3];
                mpedge[{std::min(e0, e1), std::max(e0, e1)}].push_back({e0, e1, o});
            }
        }
        std::vector<std::array<int, 4>> aedge;
        for (const auto& [key, a] : mpedge)
            if (a.size() == 2 && a[0][0] == a[1][1] && a[0][1] == a[1][0]) aedge.push_back({a[0][2], a[0][0], a[0][1], a[1][2]});
        struct STRIP { std::vector<int> apos; std::map<int, int> mp; std::vector<std::array<uint8_t, 4>> aedge; };
        std::vector<STRIP> astrip(1);
        for (const auto& e : aedge)
        {
            STRIP* ps = &astrip.back();
            int cnew = 0;
            for (int v : e) if (!ps->mp.count(v)) ++cnew;
            if (ps->apos.size() + size_t(cnew) > 255 || ps->aedge.size() >= 255) { astrip.emplace_back(); ps = &astrip.back(); }
            std::array<uint8_t, 4> r{};
            for (int k = 0; k < 4; ++k)
            {
                auto it = ps->mp.find(e[size_t(k)]);
                if (it == ps->mp.end()) { it = ps->mp.emplace(e[size_t(k)], int(ps->apos.size())).first; ps->apos.push_back(e[size_t(k)]); }
                r[size_t(k)] = uint8_t(it->second);
            }
            ps->aedge.push_back(r);
        }
        if (astrip.back().aedge.empty()) astrip.pop_back();
        w.put<int16_t>(int16_t(astrip.size()));
        const int cw = std::min<int>(int(g.abone.size()), 4);
        for (const STRIP& st : astrip)
        {
            w.put<uint8_t>(uint8_t(st.apos.size()));
            for (int v : st.apos) w.vec(g.avtx[size_t(aifirst[size_t(v)])].pos);
            if (!g.abone.empty())
            {
                w.put<uint8_t>(uint8_t(cw));
                for (int k = 0; k < cw; ++k) w.put<uint8_t>(uint8_t(k));
                for (int v : st.apos)
                    for (int k = 0; k < cw; ++k) w.put<uint8_t>(g.avtx[size_t(aifirst[size_t(v)])].aw[k]);
            }
            w.put<uint8_t>(uint8_t(st.aedge.size()));
            for (const auto& r : st.aedge) w.bytes(r.data(), 4);
        }
    }
    w.put<int16_t>(0);   // sphere sets
    return w.ab;
}

// ---- Graphics: new CLUTs, BMPs and shaders appended to a group's lists (indices after the existing ones)

struct NEWSHD
{
    GAMETEX tex;
    int shdk = 1, grfshd = 2, grftex = 0;
    uint32_t argba[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0};
    int id = 0;   // the shader's (its BMP / CLUT use id + 1 / + 2)
};

bool FAppendGraphics(std::vector<uint8_t>& ab, const std::vector<NEWSHD>& anew, std::vector<int>& aishd, std::string& error)
{
    GFXINFO gi;
    if (!FParseGraphics(ab, gi, error)) return false;
    const int cclut0 = int(gi.aclut.size()), cbmp0 = int(gi.abmp.size()), cshd0 = int(gi.ashd.size());
    if (cshd0 + int(anew.size()) > 0xFFFE || cbmp0 + int(anew.size()) > 0xFFFE || cclut0 + 2 * int(anew.size()) > 0xFFFE) { error = "too many shaders in the group"; return false; }
    WR wc, wb, ws;
    aishd.clear();
    int cclutNew = 0;
    for (std::size_t i = 0; i < anew.size(); ++i)
    {
        const NEWSHD& n = anew[i];
        // CLUT records (16 bytes) + colours: the colours, and for a lit shader the greys (id + 3)
        const int iclutColour = cclut0 + cclutNew;
        for (int g = 0; g < (n.shdk == 1 ? 2 : 1); ++g)
        {
            const std::vector<uint32_t> aclut = g ? n.tex.GreyClut() : n.tex.aclut;
            wc.put<int32_t>(n.id + (g ? 3 : 2));
            wc.put<uint8_t>(TEXK_Streamed);
            wc.put<int8_t>(0);
            wc.put<uint16_t>(0);
            wc.put<uint16_t>(uint16_t(aclut.size()));
            wc.put<uint16_t>(0);
            wc.put<uint32_t>(0);
            wc.bytes(aclut.data(), aclut.size() * 4);
            ++cclutNew;
        }
        // BMP record (24 bytes) + pixels
        wb.put<int32_t>(n.id + 1);
        wb.put<uint8_t>(TEXK_Streamed);
        wb.put<int8_t>(0);
        wb.put<uint16_t>(0);
        wb.put<uint16_t>(uint16_t(n.tex.dx));
        wb.put<uint16_t>(uint16_t(n.tex.dy));
        wb.put<uint8_t>(n.tex.fT4 ? 0x14 : 0x13);
        wb.put<uint8_t>(uint8_t(std::max(1, n.tex.dx / 64)));
        wb.put<uint16_t>(0);
        wb.put<uint32_t>(uint32_t(n.tex.apix.size()));
        wb.put<uint32_t>(0);
        wb.bytes(n.tex.apix.data(), n.tex.apix.size());
        // shader (0x18 bytes), no animation, one texture state: the BMP, the CLUT for each pass
        ws.put<int32_t>(n.id);
        ws.put<uint8_t>(uint8_t(n.shdk));
        ws.put<uint8_t>(uint8_t(n.grfshd));
        ws.put<uint8_t>(0);
        ws.put<uint8_t>(0);
        for (uint32_t c : n.argba) ws.put<uint32_t>(c);
        ws.put<uint16_t>(0);
        ws.put<uint8_t>(0);
        ws.put<uint8_t>(1);
        ws.put<int16_t>(-1);
        const int cclutPass = n.shdk == 1 ? 2 : n.shdk == 7 ? ((n.grfshd & 0x20) ? 2 : 3) : 1;
        ws.put<int32_t>(n.id);
        ws.put<uint16_t>(uint16_t(n.grftex));
        ws.put<uint8_t>(1);
        ws.put<uint8_t>(uint8_t(cclutPass));
        ws.put<uint16_t>(uint16_t(cbmp0 + int(i)));
        for (int k = 0; k < cclutPass; ++k) ws.put<uint16_t>(uint16_t(iclutColour + (n.shdk == 1 && k == 1 ? 1 : 0)));
        aishd.push_back(cshd0 + int(i));
    }
    auto u16at = [&](std::size_t ib) { uint16_t v; std::memcpy(&v, ab.data() + ib, 2); return v; };
    std::vector<uint8_t> out;
    out.reserve(ab.size() + wc.ab.size() + wb.ab.size() + ws.ab.size());
    auto copy = [&](std::size_t a, std::size_t b) { out.insert(out.end(), ab.begin() + std::ptrdiff_t(a), ab.begin() + std::ptrdiff_t(b)); };
    auto putu16 = [&](uint16_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); };
    putu16(uint16_t(u16at(0) + cclutNew));
    copy(2, gi.ibCbmp);
    out.insert(out.end(), wc.ab.begin(), wc.ab.end());
    putu16(uint16_t(u16at(gi.ibCbmp) + anew.size()));
    copy(gi.ibCbmp + 2, gi.ibCsaa);
    out.insert(out.end(), wb.ab.begin(), wb.ab.end());
    copy(gi.ibCsaa, gi.ibCshd);
    putu16(uint16_t(u16at(gi.ibCshd) + anew.size()));
    copy(gi.ibCshd + 2, gi.ibEndShd);
    out.insert(out.end(), ws.ab.begin(), ws.ab.end());
    copy(gi.ibEndShd, ab.size());
    ab = std::move(out);
    return true;
}

// ---- the editor's side

bool CEditor::FFieldRange(LO* plo, EDITFK fk, std::string& rel, std::size_t& ib, std::size_t& cb) const
{
    const auto it = m_mpobj.find(plo);
    if (it == m_mpobj.end()) return false;
    const int ifield = fk == EDITFK_Globset ? it->second.iglobset : fk == EDITFK_Soes ? it->second.isoes : -1;
    if (ifield < 0) return false;
    const EDITFIELD& f = m_rec.afield[size_t(ifield)];
    cb = f.ibEnd - f.ib;
    return FLocate(f.ib, cb, rel, ib);
}

std::string CEditor::RelGraphicsOf(const std::string& relObjects) const
{
    const auto slash = relObjects.rfind('/');
    if (slash == std::string::npos) return std::string();
    const std::string rel = relObjects.substr(0, slash) + "/Graphics.bin";
    return m_proj.partRef(rel) ? rel : std::string();
}

void CEditor::PrintModelInfo(LO* plo)
{
    std::string rel, err;
    std::size_t ib = 0, cb = 0;
    if (!plo || !FFieldRange(plo, EDITFK_Globset, rel, ib, cb)) { Status("model: the selection has no stored model"); return; }
    const std::vector<uint8_t>* pab = m_proj.partRef(rel);
    GLOBSETINFO gi;
    if (!pab || !FParseGlobset(pab->data() + ib, cb, gi, err)) { Status("model: " + err); return; }
    std::printf("editor: model of %d: %s +%zu (%zu bytes) nodraw %d center %.1f %.1f %.1f r %.1f saa %d ids %zu globs %zu spheresets %d\n", plo->oid,
                rel.c_str(), ib, cb, int(gi.fNoDraw), gi.posCenter.x, gi.posCenter.y, gi.posCenter.z, gi.sRadius, gi.csaa, gi.aids.size(), gi.aglob.size(), gi.csphereset);
    for (const GLOBINFO& g : gi.aglob)
    {
        std::string shd;
        for (int i : g.setishd) shd += " " + std::to_string(i);
        std::printf("editor:   glob mesh %d sub %d owner %d rp %d z %g fog %g n0 %d rtck %d hidden %d r %.1f u14 0x%X mrd %g grf 0x%X saa %d spec %d bones %d batches %d vtx %d tri %d shaders%s\n",
                    g.meshk, g.csub, g.oidOwner, g.rp, g.gZOrder, g.uFog, g.n0, g.rtck, g.fHidden, g.sRadius, g.u14, g.sMRD, g.grfMesh, g.isaa, g.iSpec,
                    g.cbone, g.cbatch, g.cvtx, g.ctri, shd.c_str());
    }
    // the live shaders and a vertex
    ALO* palo = dynamic_cast<ALO*>(plo);
    if (palo && palo->pglobset)
        for (const GLOB& g : palo->pglobset->aglob)
            for (const SUBGLOB& sub : g.asubglob)
            {
                if (!sub.pshd || sub.cvtx == 0) continue;
                const VTXGLOB& v = g.avtx[sub.ivtxFirst];
                const TEXSEL sel = TexselFromShd(sub.pshd, 0, 0);
                for (int ip = 0; ip < 3; ++ip)
                {
                    const TEXSEL s2 = TexselFromShd(sub.pshd, 0, ip);
                    if (!s2.pclut || !s2.pclut->pargba) continue;
                    double m[4] = {};
                    for (uint32_t c : *s2.pclut->pargba) for (int k = 0; k < 4; ++k) m[k] += (c >> (8 * k)) & 0xFF;
                    const double n = double(s2.pclut->pargba->size());
                    std::printf("editor:     pass %d clut mean %.0f %.0f %.0f %.0f\n", ip, m[0] / n, m[1] / n, m[2] / n, m[3] / n);
                }
                std::printf("editor:   entry grf 0x%X shd id 0x%X kind %d grf 0x%X rgba %08X %08X %08X tex %dx%d psm 0x%X clut %d | vtx rgbaSrc %08X A %08X B %08X uv %.3f %.3f n %.2f %.2f %.2f\n",
                            sub.grf, sub.pshd->id, sub.pshd->shdk, sub.pshd->grfshd, sub.pshd->argba[0], sub.pshd->argba[1], sub.pshd->argba[2],
                            sel.pbmp ? sel.pbmp->dx : 0, sel.pbmp ? sel.pbmp->dy : 0, sel.pbmp ? sel.pbmp->psm : 0, sel.pclut ? sel.pclut->ccolor : 0,
                            v.rgbaSrc, v.rgbaA, v.rgbaB, v.uv.x, v.uv.y, v.normal.x, v.normal.y, v.normal.z);
            }
    {
        std::string relS;
        std::size_t ibS = 0, cbS = 0;
        SOESINFO si;
        if (FFieldRange(plo, EDITFK_Soes, relS, ibS, cbS) && FParseSoes(m_proj.partRef(relS)->data() + ibS, cbS, si, err))
        {
            std::printf("editor:   collision: %zu entries (%zu bytes) mass %g\n", si.asoe.size(), cbS, si.gMass);
            for (const SOEINFO& e : si.asoe)
            {
                std::string sb;
                for (std::size_t k = 0; k < std::min<std::size_t>(e.abspn.size(), 8); ++k)
                    sb += " [" + std::to_string(int(e.abspn[k][0])) + " " + std::to_string(int(e.abspn[k][1])) + " " + std::to_string(int(e.abspn[k][2])) + "]";
                std::printf("editor:     soe xf %d tag %d f48 %d f49 %d r %.1f pos %d surf %d edge %d bsp %u x4 %u x8 %u id %u state %d pinned %d f50 %d box %.1f %.1f %.1f .. %.1f %.1f %.1f bsp%s\n",
                            e.ixf, e.tag, e.f48, e.f49, e.sRadius, e.cpos, e.csurf, e.cedge, e.cbsp, e.x4, e.x8, e.id, e.state, e.pinned, e.f50,
                            e.posMin.x, e.posMin.y, e.posMin.z, e.posMax.x, e.posMax.y, e.posMax.z, sb.c_str());
            }
        }
        else std::printf("editor:   collision: %s\n", err.c_str());
    }
    const std::string relG = RelGraphicsOf(rel);
    GFXINFO gx;
    if (!relG.empty() && FParseGraphics(*m_proj.partRef(relG), gx, err))
    {
        std::printf("editor:   graphics %s: %zu CLUTs %zu BMPs %zu shaders (end %zu of %zu)\n", relG.c_str(), gx.aclut.size(), gx.abmp.size(), gx.ashd.size(),
                    gx.ibEndShd, m_proj.partRef(relG)->size());
        for (const GFXCLUT& c : gx.aclut) std::printf("editor:     clut id 0x%X texk %d colours %d\n", c.id, c.texk, c.ccolor);
        for (const GFXBMP& b : gx.abmp) std::printf("editor:     bmp id 0x%X texk %d %dx%d psm 0x%X cb %u\n", b.id, b.texk, b.dx, b.dy, b.psm, b.cb);
        for (const GFXSHD& h : gx.ashd)
            for (const GFXTEX& t : h.atex)
            {
                std::string sb, sc;
                for (int i : t.aibmp) sb += " " + std::to_string(i);
                for (int i : t.aiclut) sc += " " + std::to_string(i);
                std::printf("editor:     shd id 0x%X kind %d grf 0x%X saa %d tex oid %d grftex 0x%X bmps%s cluts%s\n", h.id, h.shdk, h.grfshd, h.isaa, t.oid, t.grftex, sb.c_str(), sc.c_str());
            }
    }
    else std::printf("editor:   graphics: %s\n", relG.empty() ? "none (inline)" : err.c_str());
}

// ---- import

namespace
{
// glTF (Y up, metres) -> the game (Z up): (x, y, z) -> (x, -z, y) x scale
glm::mat3 AxisToGame() { return glm::mat3(glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, -1, 0)); }

uint32_t PackRgba(const glm::vec4& c)
{
    uint32_t r = 0;
    for (int k = 0; k < 4; ++k) r |= uint32_t(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f) << (8 * k);
    return r;
}

// "<name>#<oid>.<record>": a joint exported from the game (FExportGlb)
bool FBoneOfJointName(const std::string& name, int& oid, int& ixf)
{
    const auto hash = name.rfind('#');
    if (hash == std::string::npos) return false;
    return std::sscanf(name.c_str() + hash + 1, "%d.%d", &oid, &ixf) == 2;
}

int IdUnused(const GFXINFO& gi, uint32_t seed)
{
    std::set<int> setId;
    for (const GFXSHD& h : gi.ashd) setId.insert(h.id);
    for (const GFXBMP& b : gi.abmp) setId.insert(b.id);
    for (const GFXCLUT& c : gi.aclut) setId.insert(c.id);
    int id = int(0x7E000000u | ((seed & 0x3FFFFFu) << 2));
    while (setId.count(id) || setId.count(id + 1) || setId.count(id + 2) || setId.count(id + 3)) id += 4;
    return id;
}
}  // namespace

bool CEditor::FImportModel(const std::string& path, LO* plo, bool fNew, std::string& error)
{
    md::Scene sc;
    if (!md::load(ce::pathOf(path), sc, error)) return false;
    // a new object: a copy of the selected plain object (or of the level's first plain ALO with a model) in front of
    // the camera, then its model replaced
    if (fNew)
    {
        int iobjSrc = -1;
        auto fPlain = [&](const EDITOBJREC& r) {
            // (not one the world takes out at its start, option 26: the copy would start out of the world too)
            if (r.depth != 0 || !r.plo || r.ibMatrix || !FIsLoDerivedFrom(r.plo, 0x30) || r.plo->fRemoveAtStart) return false;
            const auto it = m_mpobj.find(r.plo);
            return it != m_mpobj.end() && it->second.iglobset >= 0 && it->second.imat >= 0;
        };
        // (the selection carries it when it is a plain object of a level group: a shared group's would make a template)
        if (plo)
        {
            const int i = IobjOfLo(PloTopLevel(plo));
            const int ig = i >= 0 ? m_rec.aobj[size_t(i)].plo->igrp : -1;
            if (i >= 0 && fPlain(m_rec.aobj[size_t(i)]) && ig >= 0 && ig < int(m_index.groups.size()) && m_index.groups[size_t(ig)].level) iobjSrc = i;
            else plo = nullptr;
        }
        // (else a plain ALO: the smallest record of that class, anywhere; it is copied into the level group)
        std::size_t cbBest = SIZE_MAX;
        for (std::size_t i = 0; plo == nullptr && i < m_rec.aobj.size(); ++i)
        {
            const EDITOBJREC& r = m_rec.aobj[i];
            if (fPlain(r) && std::string(SzClassRaw(r.cid)) == "ALO" && r.cb < cbBest) { cbBest = r.cb; iobjSrc = int(i); }
        }
        // (into a level group that has a texture block, the imported textures go there)
        int igrpLevel = -1;
        for (const ce::GroupInfo& g : m_index.groups)
            if (g.level && !g.objectsPart.empty() && !RelGraphicsOf(g.objectsPart).empty()) { igrpLevel = g.index; break; }
        if (plo && iobjSrc >= 0 && RelGraphicsOf(m_index.groups[size_t(m_rec.aobj[size_t(iobjSrc)].plo->igrp)].objectsPart).empty()) plo = nullptr;
        if (!plo && igrpLevel < 0) { error = "no level group with a texture block"; return false; }
        if (iobjSrc < 0) { error = "no plain object to carry the model (select one)"; return false; }
        glm::vec3 pos = m_cam.pos + m_cam.Dir() * 600.0f, hit;
        if (FSnapDown(pos, hit)) pos = hit;
        const int oidNew = CopyRecord(iobjSrc, plo ? m_rec.aobj[size_t(iobjSrc)].plo->igrp : igrpLevel, &pos, true, error);
        if (!oidNew || !m_plSel) { if (error.empty()) error = "the copy failed"; return false; }
        plo = m_plSel;
        m_proj.begin("Import " + ce::pathOf(path).filename().string() + " (new object " + std::to_string(oidNew) + ")", true);
    }
    else m_proj.begin("Import " + ce::pathOf(path).filename().string() + " into " + std::to_string(plo ? plo->oid : 0), true);
    struct ENDER { ce::Project& p; bool f = true; ~ENDER() { if (f) p.end(); } } ender{m_proj};
    ALO* palo = dynamic_cast<ALO*>(plo);
    std::string rel, relS;
    std::size_t ib = 0, cb = 0, ibS = 0, cbS = 0;
    if (!palo || !FFieldRange(plo, EDITFK_Globset, rel, ib, cb)) { error = "the selection has no stored model to replace"; return false; }
    const std::string relG = RelGraphicsOf(rel);
    if (relG.empty()) { error = "its group has no texture block (inline Graphics part): import into an object of a group with textures"; return false; }
    std::vector<uint8_t> abObj, abG;
    m_proj.readPart(rel, abObj);
    m_proj.readPart(relG, abG);
    GLOBSETINFO gold;
    if (!FParseGlobset(abObj.data() + ib, cb, gold, error)) return false;
    GFXINFO gx;
    if (!FParseGraphics(abG, gx, error)) return false;
    // the replaced model's header fields and shader flags
    GLOBHDROUT hdr;
    hdr.n0 = plo->oid;
    int grfshdRef = 2, oidOwnerSkin = -1;
    for (const GLOBINFO& g : gold.aglob)
        if (g.meshk == 0)
        {
            hdr.rp = g.rp; hdr.n0 = g.n0; hdr.rtck = g.rtck; hdr.u14 = g.u14; hdr.uFog = g.uFog;
            if (!g.setishd.empty() && *g.setishd.begin() < int(gx.ashd.size())) grfshdRef = gx.ashd[size_t(*g.setishd.begin())].grfshd;
            break;
        }
    for (const GLOBINFO& g : gold.aglob)
        if (g.cbone > 0) { oidOwnerSkin = g.oidOwner; break; }
    const glm::mat4 matObj = MatWorld(plo);
    const glm::mat3 R = AxisToGame();
    const float scale = m_importScale;
    // skinned meshes: joints named "<name>#oid.record" bound to the object's records (else drawn at the bind pose)
    std::vector<std::pair<int, int>> abonejoint(sc.joints.size(), {-1, -1});
    bool fSkinOk = !sc.joints.empty();
    for (std::size_t j = 0; j < sc.joints.size(); ++j)
    {
        int oid = 0, ixf = 0;
        if (!FBoneOfJointName(sc.joints[j].name, oid, ixf)) { fSkinOk = false; continue; }
        abonejoint[j] = {oid, ixf};
    }
    const XF* pxfOwner = oidOwnerSkin >= 0 ? PxfFindAlo(palo, oidOwnerSkin) : nullptr;
    const glm::mat4 matToOwner = (pxfOwner ? glm::inverse(pxfOwner->matWorld) : glm::inverse(matObj)) * matObj;
    // materials -> new shaders (the ones used)
    std::vector<int> aimatUsed(sc.materials.size() + 1, -1);
    std::vector<NEWSHD> anew;
    std::vector<md::Image> afull;
    auto shaderOf = [&](int imat) -> int {
        const int key = imat < 0 ? int(sc.materials.size()) : imat;
        if (aimatUsed[size_t(key)] >= 0) return aimatUsed[size_t(key)];
        md::Material mat;
        if (imat >= 0) mat = sc.materials[size_t(imat)];
        md::Image img;
        if (mat.image >= 0 && mat.image < int(sc.images.size())) img = sc.images[size_t(mat.image)];
        else { img.w = img.h = 8; img.rgba.assign(8 * 8 * 4, 255); img.name = mat.name.empty() ? "colour" : mat.name; }
        for (std::size_t k = 0; k < img.rgba.size(); ++k)
            img.rgba[k] = uint8_t(std::clamp(img.rgba[k] * mat.base[int(k % 4)], 0.0f, 255.0f) + 0.5f);
        NEWSHD n;
        n.tex = GametexFromImage(img, m_importMaxTex);
        n.shdk = m_importLit ? 1 : 0;
        n.grfshd = grfshdRef;
        n.id = IdUnused(gx, uint32_t(std::hash<std::string>()(path + "|" + std::to_string(key))) + uint32_t(anew.size()));
        GFXSHD hId; hId.id = n.id; gx.ashd.push_back(hId);   // (the next id avoids it)
        GFXBMP bId; bId.id = n.id + 1; gx.abmp.push_back(bId);
        GFXCLUT cId; cId.id = n.id + 2; gx.aclut.push_back(cId);
        cId.id = n.id + 3; gx.aclut.push_back(cId);
        anew.push_back(n);
        afull.push_back(img);
        aimatUsed[size_t(key)] = int(anew.size()) - 1;
        return int(anew.size()) - 1;
    };
    // the globs: per material (and, skinned, per set of up to four bones)
    std::vector<GLOBOUT> aglob;
    std::map<std::pair<int, std::vector<std::pair<int, int>>>, int> mpglob;   // (only the key's shader / bones-or-not used)
    std::size_t ctri = 0;
    bool fSkinned = false;
    // (a file exported from several objects of the game: the parts named for this object, when it has any)
    auto oidOfMesh = [](const std::string& name) {
        const auto at = name.rfind('@');
        const auto us2 = name.rfind('_', at == std::string::npos ? std::string::npos : at);
        const auto us1 = us2 == std::string::npos || us2 == 0 ? std::string::npos : name.rfind('_', us2 - 1);
        return us1 == std::string::npos ? INT_MIN : std::atoi(name.c_str() + us1 + 1);
    };
    bool fOwnParts = false;
    for (const md::Mesh& mesh : sc.meshes) fOwnParts |= oidOfMesh(mesh.name) == plo->oid;
    for (const md::Mesh& mesh : sc.meshes)
    {
        if (fOwnParts && oidOfMesh(mesh.name) != plo->oid) continue;
        const bool fSkin = mesh.skinned && fSkinOk;
        // "<name>@n0,rp,hidden": a part exported from the game keeps its show id, draw order and hidden flag
        int n0Mesh = INT_MIN, rpMesh = -1, fHiddenMesh = 0;
        if (const auto at = mesh.name.rfind('@'); at != std::string::npos)
            std::sscanf(mesh.name.c_str() + at + 1, "%d,%d,%d", &n0Mesh, &rpMesh, &fHiddenMesh);
        for (const md::Primitive& prim : mesh.prims)
        {
            const int inew = shaderOf(prim.material);
            const bool fBlend = prim.material >= 0 && sc.materials[size_t(prim.material)].blend;
            for (std::size_t t = 0; t + 2 < prim.idx.size(); t += 3)
            {
                VTXOUT av[3];
                std::vector<std::pair<int, int>> abone;   // the triangle's bones (up to four, by weight)
                if (fSkin && prim.joints.size() == prim.pos.size() && prim.weights.size() == prim.pos.size())
                {
                    std::map<int, float> mpw;
                    for (int k = 0; k < 3; ++k)
                        for (int q = 0; q < 4; ++q) mpw[prim.joints[prim.idx[t + size_t(k)]][q]] += prim.weights[prim.idx[t + size_t(k)]][q];
                    std::vector<std::pair<float, int>> aw;
                    for (auto& [j, wsum] : mpw) if (wsum > 0) aw.push_back({-wsum, j});
                    std::sort(aw.begin(), aw.end());
                    if (aw.size() > 4) aw.resize(4);
                    for (auto& [wneg, j] : aw) abone.push_back(abonejoint[size_t(j)]);
                    std::sort(abone.begin(), abone.end());
                }
                for (int k = 0; k < 3; ++k)
                {
                    const uint32_t iv = prim.idx[t + size_t(k)];
                    VTXOUT& v = av[k];
                    glm::vec3 p = R * prim.pos[iv] * scale;
                    glm::vec3 n = prim.nrm.size() == prim.pos.size() ? R * prim.nrm[iv] : glm::vec3(0, 0, 1);
                    if (!abone.empty()) { p = glm::vec3(matToOwner * glm::vec4(p, 1.0f)); n = glm::mat3(matToOwner) * n; }
                    v.pos = p;
                    const float ln = glm::length(n);
                    v.nrm = ln > 0 ? n / ln : glm::vec3(0, 0, 1);
                    if (prim.uv.size() == prim.pos.size()) v.uv = glm::vec2(prim.uv[iv].x, 1.0f - prim.uv[iv].y);
                    glm::vec4 col = prim.color.size() == prim.pos.size() ? prim.color[iv] : glm::vec4(1.0f);
                    if (!m_importLit) col = glm::vec4(glm::vec3(col) * (128.0f / 255.0f), 1.0f);   // (unlit: 0x80 is 1.0)
                    else col.a = 1.0f;
                    v.rgba = PackRgba(col);
                    if (!abone.empty())
                    {
                        float aw[4] = {}, wsum = 0;
                        for (int q = 0; q < 4; ++q)
                        {
                            const std::pair<int, int> b = abonejoint[prim.joints[iv][q]];
                            const auto it = std::find(abone.begin(), abone.end(), b);
                            if (it != abone.end()) { aw[it - abone.begin()] += prim.weights[iv][q]; wsum += prim.weights[iv][q]; }
                        }
                        int acc = 0;
                        for (int q = 0; q < int(abone.size()); ++q)
                        {
                            v.aw[q] = uint8_t(wsum > 0 ? std::clamp(aw[q] / wsum * 255.0f + 0.5f, 0.0f, 255.0f) : (q == 0 ? 255 : 0));
                            acc += v.aw[q];
                        }
                        if (acc != 255 && !abone.empty()) v.aw[0] = uint8_t(std::clamp(int(v.aw[0]) + 255 - acc, 0, 255));
                    }
                }
                // a glob of this shader whose bones (with the triangle's) stay within four
                int ig = -1;
                for (auto& [key, i] : mpglob)
                {
                    const GLOBOUT& gk = aglob[size_t(i)];
                    if (gk.ishd != inew || gk.fBlend != fBlend || (gk.abone.empty() != abone.empty()) || gk.n0 != n0Mesh || gk.rp != rpMesh ||
                        gk.fHidden != (fHiddenMesh != 0))
                        continue;
                    std::vector<std::pair<int, int>> u = aglob[size_t(i)].abone;
                    for (const auto& b : abone) if (std::find(u.begin(), u.end(), b) == u.end()) u.push_back(b);
                    if (u.size() <= 4) { ig = i; break; }
                }
                if (ig < 0)
                {
                    ig = int(aglob.size());
                    aglob.emplace_back();
                    aglob.back().ishd = inew;
                    aglob.back().fBlend = fBlend;
                    aglob.back().n0 = n0Mesh;
                    aglob.back().rp = rpMesh;
                    aglob.back().fHidden = fHiddenMesh != 0;
                    aglob.back().oidOwner = abone.empty() ? -1 : oidOwnerSkin;
                    aglob.back().fOutline = m_importOutline;
                    mpglob.emplace(std::make_pair(inew, std::vector<std::pair<int, int>>{std::make_pair(int(aglob.size()), int(abone.size()))}), ig);
                }
                GLOBOUT& g = aglob[size_t(ig)];
                // the triangle's bone slots as the glob's
                for (const auto& b : abone) if (std::find(g.abone.begin(), g.abone.end(), b) == g.abone.end()) g.abone.push_back(b);
                for (VTXOUT& v : av)
                {
                    uint8_t aw2[4] = {}, aj2[4] = {};
                    for (int q = 0; q < int(abone.size()); ++q)
                    {
                        const int slot = int(std::find(g.abone.begin(), g.abone.end(), abone[size_t(q)]) - g.abone.begin());
                        aw2[slot] = v.aw[q];
                        aj2[slot] = uint8_t(slot);
                    }
                    std::memcpy(v.aw, aw2, 4);
                    std::memcpy(v.aj, aj2, 4);
                    g.avtx.push_back(v);
                }
                if (!abone.empty()) fSkinned = true;
                ++ctri;
            }
        }
    }
    if (aglob.empty()) { error = "the file has no triangles"; return false; }
    if (aglob.size() > 32000) { error = "too many parts"; return false; }
    // (a glob's earlier vertices keep their weights when a later triangle added a bone: slots only grow)
    std::vector<int> aishd;
    if (!FAppendGraphics(abG, anew, aishd, error)) return false;
    for (GLOBOUT& g : aglob) g.ishd = aishd[size_t(g.ishd)];
    // (skinned globs keep the replaced model's owner record: their vertices are in its space; static ones the object's)
    const std::vector<uint8_t> abGlob = GlobsetBytes(aglob, hdr);
    {
        GLOBSETINFO gcheck;
        if (!FParseGlobset(abGlob.data(), abGlob.size(), gcheck, error)) { error = "internal: the written model does not parse: " + error; return false; }
    }
    // collision: kept, none, or a box around the model (the replaced object's first entry's fields)
    if (m_importCollision != 0 && FFieldRange(plo, EDITFK_Soes, relS, ibS, cbS) && relS == rel)
    {
        SOESINFO si;
        std::string e2;
        if (!FParseSoes(abObj.data() + ibS, cbS, si, e2)) { error = "collision: " + e2; return false; }
        WR w;
        if (m_importCollision == 1) w.put<int16_t>(0);
        else
        {
            glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
            for (const GLOBOUT& g : aglob)
                for (const VTXOUT& v : g.avtx) { mn = glm::min(mn, v.pos); mx = glm::max(mx, v.pos); }
            SOEINFO tmpl;
            tmpl.tag = plo->oid; tmpl.state = -10; tmpl.pinned = 8; tmpl.x4 = 6; tmpl.ixf = 0;
            if (!si.asoe.empty()) { tmpl = si.asoe[0]; tmpl.pinned = 8; tmpl.x4 = 6; }
            if (m_importCollision == 3 && !m_aconvexImport.empty())
            {
                // a brush's convex pieces
                w.put<int16_t>(int16_t(m_aconvexImport.size()));
                for (const CONVEX& cv : m_aconvexImport) WriteConvexSoe(w, tmpl, cv);
            }
            else
            {
                w.put<int16_t>(1);
                WriteBoxSoe(w, tmpl, mn, mx);
            }
            w.put<float>(si.asoe.empty() ? 1.0f : si.gMass);
            const float aI[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            for (int k = 0; k < 9; ++k) w.put<float>(si.asoe.empty() ? aI[k] : si.agInertia[k]);
        }
        if (!m_proj.spliceBytes(rel, ibS, cbS, w.ab, EnclosingSizeFields(rel, ibS))) { error = "the collision could not be written"; return false; }
    }
    if (!m_proj.spliceBytes(rel, ib, cb, abGlob, EnclosingSizeFields(rel, ib))) { error = "the model could not be written"; return false; }
    if (!m_proj.setPart(relG, abG)) { error = "the textures could not be written"; return false; }
    // the full images for the texture pack path (the game copy is at most m_importMaxTex; the package's Textures/
    // has the original resolution, used in its place by the renderer)
    int cfull = 0;
    for (std::size_t i = 0; i < anew.size(); ++i)
    {
        if (afull[i].w <= anew[i].tex.dx && afull[i].h <= anew[i].tex.dy) continue;
        const std::string name = SzModNameOf(anew[i].tex);
        if (name.empty()) continue;
        m_proj.setPart("Textures/" + name, md::pngBytes(afull[i]));
        if (anew[i].shdk == 1)
            if (const std::string nameGrey = SzModNameOf(anew[i].tex, nullptr, true); !nameGrey.empty() && nameGrey != name)
                m_proj.setPart("Textures/" + nameGrey, md::pngBytes(GreyImage(afull[i])));
        ++cfull;
    }
    ender.f = false;
    m_proj.end();
    const int oid = plo->oid;
    char sz[256];
    std::snprintf(sz, sizeof(sz), "imported %s: %zu triangles, %zu parts, %zu textures (%d full size in Textures/)%s into %d", ce::pathOf(path).filename().string().c_str(),
                  ctri, aglob.size(), anew.size(), cfull, fSkinned ? ", skinned" : (sc.joints.empty() ? "" : ", bind pose (joints not from this game)"), oid);
    Reload(oid);
    Status(sz);
    return true;
}

// ---- materials (the shaders an object's model uses) and their textures

namespace
{
// the object's shaders in the order its globs use them
std::vector<SHD*> ApshdOfAlo(const ALO* palo)
{
    std::vector<SHD*> a;
    if (!palo || !palo->pglobset) return a;
    for (const GLOB& g : palo->pglobset->aglob)
    {
        const GLOB& geom = g.iglobTemplate >= 0 && g.asubglob.empty() ? palo->pglobset->aglob[size_t(g.iglobTemplate)] : g;
        for (const SUBGLOB& sub : geom.asubglob)
            if (sub.pshd && std::find(a.begin(), a.end(), sub.pshd) == a.end()) a.push_back(sub.pshd);
    }
    return a;
}
// the texture pass that holds the shader's colours (kind 1 / 7: pass 0, the lit pass's; pass 1 is the darker ambient
// palette of the same bitmap)
int ItexColour(const SHD*) { return 0; }
}  // namespace

// The preview of the project's Textures/ (the texture-pack path): written to a temporary folder and read by the renderer
void CEditor::ApplyProjectTextures()
{
    std::error_code ec;
    static const std::string s_stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFF);
    const fs::path dir = fs::temp_directory_path(ec) / ("safehouse-editor-" + s_stamp);
    fs::remove_all(dir, ec);
    int c = 0;
    for (const std::string& rel : m_proj.changedParts())
    {
        if (rel.rfind("Textures/", 0) != 0) continue;
        std::vector<uint8_t> ab;
        if (m_proj.readPart(rel, ab) && ce::writeFile(dir / ce::pathOf(rel), ab)) ++c;
    }
    ReloadTextureModsFrom(c ? std::vector<std::string>{cane::utf8(dir)} : std::vector<std::string>{});
    ResetTextureCache();
}

bool CEditor::FImportTexture(const std::string& png, LO* plo, int imat, int mode, std::string& error)
{
    ALO* palo = dynamic_cast<ALO*>(plo);
    const std::vector<SHD*> apshd = ApshdOfAlo(palo);
    if (imat < 0 || imat >= int(apshd.size())) { error = "the selection has no material " + std::to_string(imat); return false; }
    SHD* pshd = apshd[size_t(imat)];
    md::Image img;
    if (!md::loadImage(ce::pathOf(png), img, error)) return false;
    if (mode == 0)
    {
        // the texture-pack path: the PNG at its own size under each name the shader's textures decode to
        m_proj.begin("Texture " + ce::pathOf(png).filename().string() + " for material " + std::to_string(imat) + " of " + std::to_string(plo->oid));
        const std::vector<uint8_t> ab = md::pngBytes(img);
        const std::vector<uint8_t> abGrey = md::pngBytes(GreyImage(img));
        std::set<std::string> setName;
        for (int ipass = 0; ipass < std::max(1, int(pshd->atex.empty() ? 0 : pshd->atex[0].apclut.size())); ++ipass)
        {
            const TEXSEL sel = TexselFromShd(pshd, 0, ipass);
            std::vector<uint8_t> rgba;
            std::string e2;
            if (!sel.pbmp || !FDecodeTexture(sel.pbmp, sel.pclut, rgba, e2)) continue;
            const std::string name = SzTextureModName(rgba, sel.pbmp->dx, sel.pbmp->dy);
            bool fGrey = sel.pclut && sel.pclut->pargba;
            if (fGrey)
                for (uint32_t c : *sel.pclut->pargba)
                    if ((c & 0xFF) != ((c >> 8) & 0xFF) || (c & 0xFF) != ((c >> 16) & 0xFF)) { fGrey = false; break; }
            if (setName.insert(name).second) m_proj.setPart("Textures/" + name, fGrey ? abGrey : ab);
        }
        m_proj.end();
        if (setName.empty()) { error = "the material has no texture"; return false; }
        ApplyProjectTextures();
        Status("texture " + ce::pathOf(png).filename().string() + ": " + std::to_string(setName.size()) + " texture-pack name(s) in Textures/ (full size)");
        return true;
    }
    // the game's own data: the bitmap's pixels and its palettes re-encoded at their sizes
    std::string rel;
    std::size_t ib = 0, cb = 0;
    if (!FFieldRange(plo, EDITFK_Globset, rel, ib, cb)) { error = "no stored model"; return false; }
    const std::string relG = RelGraphicsOf(rel);
    std::vector<uint8_t> abG;
    GFXINFO gx;
    if (relG.empty() || !m_proj.readPart(relG, abG) || !FParseGraphics(abG, gx, error)) { if (error.empty()) error = "no Graphics part"; return false; }
    const auto ith = std::find_if(gx.ashd.begin(), gx.ashd.end(), [&](const GFXSHD& h) { return h.id == pshd->id; });
    if (ith == gx.ashd.end() || ith->atex.empty() || ith->atex[0].aibmp.empty()) { error = "the material's texture is not in its group's Graphics part"; return false; }
    const GFXTEX& tx = ith->atex[0];
    const int ibmp = tx.aibmp[0];
    if (ibmp >= int(gx.abmp.size())) { error = "bad bitmap index"; return false; }
    const GFXBMP& b = gx.abmp[size_t(ibmp)];
    if (b.psm != 0x13 && b.psm != 0x14) { error = "the bitmap is not indexed"; return false; }
    // (the bulk block ends the part: its size from the group's bulk header)
    std::size_t ibBulk0 = 0;
    std::size_t cbBulk = 0;
    for (const auto& part : m_index.json["parts"])
        if (part.value("kind", "") == "bulk" && m_index.relDir + "/" + part.value("file", "") != "" && part.contains("hex"))
        {
            const int igrp = part.value("group", -1);
            const std::string folder = igrp >= 0 && igrp < int(m_index.groups.size()) ? m_index.groups[size_t(igrp)].folder : "";
            if (folder.empty() || relG.find("/" + folder + "/") == std::string::npos) continue;
            const std::string hex = part.value("hex", "");
            uint32_t v = 0;
            for (int k = 0; k < 4 && hex.size() >= 8; ++k) v |= uint32_t(std::stoul(hex.substr(size_t(k) * 2, 2), nullptr, 16)) << (8 * k);
            cbBulk = v;
        }
    if (abG.size() < cbBulk) { error = "bad bulk size"; return false; }
    ibBulk0 = abG.size() - cbBulk;
    const std::size_t ibPix = b.texk == TEXK_Bulk ? ibBulk0 + b.ibBulk : b.ibData;
    const int ccolor = b.psm == 0x14 ? 16 : 256;
    const GAMETEX t = GametexFromImage(img, 4096, b.dx, b.dy, ccolor);
    if (ibPix + t.apix.size() > abG.size() || t.apix.size() > b.cb) { error = "the bitmap's pixels are outside the part"; return false; }
    std::memcpy(abG.data() + ibPix, t.apix.data(), t.apix.size());
    int cclut = 0;
    for (int ic : tx.aiclut)
    {
        if (ic == 0xFFFF || ic >= int(gx.aclut.size())) continue;
        const GFXCLUT& c = gx.aclut[size_t(ic)];
        const std::size_t ibC = c.texk == TEXK_Bulk ? ibBulk0 + c.ibBulk : c.ibData;
        if (c.ccolor != int(t.aclut.size()) || ibC + t.aclut.size() * 4 > abG.size()) continue;
        bool fGrey = true;
        for (int k = 0; k < c.ccolor && fGrey; ++k)
        {
            uint32_t v;
            std::memcpy(&v, abG.data() + ibC + size_t(k) * 4, 4);
            fGrey = (v & 0xFF) == ((v >> 8) & 0xFF) && (v & 0xFF) == ((v >> 16) & 0xFF);
        }
        const std::vector<uint32_t> aclut = fGrey ? t.GreyClut() : t.aclut;
        std::memcpy(abG.data() + ibC, aclut.data(), aclut.size() * 4);
        ++cclut;
    }
    m_proj.begin("Texture " + ce::pathOf(png).filename().string() + " written into the game data (" + std::to_string(b.dx) + "x" + std::to_string(b.dy) + ")", true);
    m_proj.setPart(relG, abG);
    m_proj.end();
    const int oid = plo->oid;
    Reload(oid);
    Status("texture written into the game data: " + std::to_string(t.dx) + "x" + std::to_string(t.dy) + ", " + std::to_string(ccolor) + " colours, " +
           std::to_string(cclut) + " palette(s)" + (b.texk == TEXK_Bulk ? " (a bitmap shared by id with an earlier group keeps that group's pixels)" : ""));
    return true;
}

bool CEditor::FSetMaterial(LO* plo, int imat, int shdk, int grfshd, const uint32_t argba[3], int grftex, std::string& error)
{
    ALO* palo = dynamic_cast<ALO*>(plo);
    const std::vector<SHD*> apshd = ApshdOfAlo(palo);
    if (imat < 0 || imat >= int(apshd.size())) { error = "no such material"; return false; }
    std::string rel;
    std::size_t ib = 0, cb = 0;
    if (!FFieldRange(plo, EDITFK_Globset, rel, ib, cb)) { error = "no stored model"; return false; }
    const std::string relG = RelGraphicsOf(rel);
    std::vector<uint8_t> abG;
    GFXINFO gx;
    if (relG.empty() || !m_proj.readPart(relG, abG) || !FParseGraphics(abG, gx, error)) { if (error.empty()) error = "no Graphics part"; return false; }
    const auto ith = std::find_if(gx.ashd.begin(), gx.ashd.end(), [&](const GFXSHD& h) { return h.id == apshd[size_t(imat)]->id; });
    if (ith == gx.ashd.end()) { error = "the material is not in its group's Graphics part"; return false; }
    const std::size_t ibShd = ith->ib;
    const std::size_t ibEnd = (ith + 1) != gx.ashd.end() ? (ith + 1)->ib : gx.ibEndShd;
    // the record rebuilt: its kind, flags, colours, then the texture states with as many CLUTs a frame as the kind needs
    WR w;
    w.bytes(abG.data() + ibShd, 0x18);
    w.ab[4] = uint8_t(shdk);
    w.ab[5] = uint8_t(grfshd);
    std::memcpy(w.ab.data() + 8, argba, 12);
    w.put<int16_t>(int16_t(ith->isaa));
    const int cclutPass = shdk == 1 ? 2 : shdk == 7 ? ((grfshd & 0x20) ? 2 : 3) : 1;
    const int cclutOld = ith->shdk == 1 ? 2 : ith->shdk == 7 ? ((ith->grfshd & 0x20) ? 2 : 3) : 1;
    for (const GFXTEX& t : ith->atex)
    {
        w.put<int32_t>(t.oid);
        w.put<uint16_t>(uint16_t(grftex));
        w.put<uint8_t>(uint8_t(t.aibmp.size()));
        std::vector<int> aiclut;
        for (std::size_t f = 0; f < t.aibmp.size(); ++f)
            for (int k = 0; k < cclutPass; ++k)
            {
                const std::size_t iold = f * size_t(cclutOld) + size_t(std::min(k, cclutOld - 1));
                aiclut.push_back(iold < t.aiclut.size() ? t.aiclut[iold] : (t.aiclut.empty() ? 0xFFFF : t.aiclut[0]));
            }
        if (t.aibmp.empty()) aiclut = t.aiclut;
        w.put<uint8_t>(uint8_t(aiclut.size()));
        for (int i : t.aibmp) w.put<uint16_t>(uint16_t(i));
        for (int i : aiclut) w.put<uint16_t>(uint16_t(i));
    }
    abG.erase(abG.begin() + std::ptrdiff_t(ibShd), abG.begin() + std::ptrdiff_t(ibEnd));
    abG.insert(abG.begin() + std::ptrdiff_t(ibShd), w.ab.begin(), w.ab.end());
    m_proj.begin("Material " + std::to_string(imat) + " of " + std::to_string(plo->oid), true);
    m_proj.setPart(relG, abG);
    m_proj.end();
    const int oid = plo->oid;
    Reload(oid);
    Status("material " + std::to_string(imat) + " changed (every model of the group using it)");
    return true;
}

// ---- a character posed as the game poses it (the export's animations)
//
// The game's pose of a character each frame (xf.h UpdateAloXfs, 0x22DE88): every transform record of the object, in
// the records' order, moves by the mode its head controller (the highest priority) gives it, and gets its world matrix
// (its reference's world x its local) at once. The clips (ASEGAs) put a channel controller on the records they
// animate (mode 5: the track's value at the clip's time); the rest are set at the load (xf.cpp InitAloXfs):
//   - the two-bone joints (ik.cpp, ALOPAIR, kind-7 controllers): the upper record of a limb (mode 13) is solved from its
//     rest pose so that the end (hand / foot) reaches the target record the clip moves (law of cosines for the bend
//     about the joint's axis, then the turn onto the target, then the twist an ASEGA's scalar channel eases); the lower
//     record (mode 14) only gets the bend. A clip that drives the upper record itself outranks the joint (prio -1).
//   - links (mode 15, xfphys.cpp): a record follows another's world position / rotation;
//   - chains (mode 16, chain.cpp): ropes / tails simulated with the clock's step;
//   - look-at (kind 2, lookat.cpp): a head turning toward the player within a range, else its loaded value;
//     look-along-motion (kind 4) and spins (kind 9).
// The renderer then skins with each bone record's world matrix x its binding (bone.cpp). So the clips' channels alone
// leave the limbs at their rest pose while the hands / feet move (the targets): the export runs the game's own update
// on the character for each clip (muted: no events, no scripts) and bakes every joint's world matrix per frame.
// Not part of a clip and left out: the look-at's target (the player), and what the actors' code does at run time on top
// (Sly's feet on the ground (slyfeet), the cane's reach (reach.cpp), a guard's arm aimed by its AI, held objects).

namespace
{
// The game's update run on a character's tree (its ALO and the ALOs under it, parents first) in the editor's world,
// which loads without the animation's start (anim.cpp InitWorldAnimation: no controllers, joints or bindings) and never
// runs it: the records' run-time state is made as the game makes it (xf.cpp InitAloXfs: their controllers, the joints'
// rest poses, links, chains; the clips' priorities and speeds, anim.cpp), each clip starts from that state, and
// everything is given back at the end (the records, their controller lists, the joints' / chains' state, the update
// flags, the clock; the controllers made here are freed).
class POSER
{
public:
    POSER(SW* psw, ALO* proot, const std::vector<ASEG*>& apaseg, bool fSolve) : m_proot(proot)
    {
        m_pswSaved = g_psw;
        g_psw = psw;
        for (const auto& up : psw->aploAll)
            if (FCidDerivedFrom(up->Cid(), 0x30) && FLoInParentChain(up.get(), proot)) m_apalo.push_back(static_cast<ALO*>(up.get()));
        if (std::find(m_apalo.begin(), m_apalo.end(), proot) == m_apalo.end()) m_apalo.insert(m_apalo.begin(), proot);
        m_clock = g_clock;
        SaveTree(m_orig);
        std::set<CTRL*> setOrig;
        for (const XFSAVE& x : m_orig.axf) setOrig.insert(x.actrl.begin(), x.actrl.end());
        // the animation's start for these objects and clips (anim.cpp InitAnimationRecordsOf without the bindings)
        for (ALO* palo : m_apalo) InitAloXfs(palo);
        InitAnimationRecordsOf(std::vector<LO*>(apaseg.begin(), apaseg.end()));
        for (ALO* palo : m_apalo)
            for (auto& up : palo->axf)
                for (CTRL* q = up->pctrlFirst; q; q = q->pnext)
                    if (!setOrig.count(q)) m_actrlMade.push_back(q);
        // no look-at (its target is the player at run time); without solving: no joints, chains or links either (the
        // clips' channels only)
        for (ALO* palo : m_apalo)
        {
            for (ALOPAIR& q : palo->apair) { q.ptrkgWeight = nullptr; q.ptWeight = nullptr; q.gWeight = 0.0f; }
            for (auto& up : palo->axf)
            {
                XF* pxf = up.get();
                std::vector<CTRL*> akeep;
                for (CTRL* q = pxf->pctrlFirst; q; q = q->pnext)
                {
                    if (q->kind == 2) continue;
                    if (!fSolve && (q->kind == 7 || q->kind == 3)) continue;
                    akeep.push_back(q);
                }
                CTRL* pheadOld = pxf->pctrlFirst;
                Relink(pxf, akeep);
                if (pxf->pctrlFirst != pheadOld)
                {
                    InvalidateXfParams(palo, pxf);
                    if (pxf->pctrlFirst) pxf->pctrlFirst->Start();
                    else { pxf->state = 0; ResetXfParams(pxf); SetXfMode(palo, pxf, 0); }
                }
                if (!fSolve && pxf->mode == XFM_Link) SetXfMode(palo, pxf, 0);
            }
        }
        SaveTree(m_start);
    }
    ~POSER()
    {
        while (!m_apasega.empty()) Stop(m_apasega.back());
        RestoreTree(m_orig);
        for (CTRL* q : m_actrlMade) delete q;
        for (ALO* palo : m_apalo) PostUpdateAloXfs(palo);   // (the world's visibility tree and activators: the object where it was)
        g_clock = m_clock;
        g_psw = m_pswSaved;
    }
    // the clip applied as the game applies it (snap, from its start; the tree in its start state), its events and
    // scripts muted, driven here only
    ASEGA* Play(ASEG* paseg)
    {
        RestoreTree(m_start);
        // (the channels' records outside the tree, if any, are kept too: the apply snaps them)
        if (ASEGD* pd = PasegdOf(paseg))
            for (const ASEGCHN& chn : pd->achn)
            {
                ALO* po = nullptr;
                XF* pxf = nullptr;
                GetAsegChannelTarget(paseg, m_proot, chn, &po, &pxf);
                if (pxf && !m_setxf.count(pxf)) { m_setxf.insert(pxf); m_orig.axf.push_back(SaveXf(pxf)); }
            }
        ASEGA* pasega = nullptr;
        g_fAsegaEventsMuted = true;
        ApplyAseg(paseg, m_proot, 0.0f, 1.0f, 0, &pasega);
        g_fAsegaEventsMuted = false;
        if (auto it = std::find(g_apasega.begin(), g_apasega.end(), pasega); it != g_apasega.end()) g_apasega.erase(it);
        if (pasega) m_apasega.push_back(pasega);
        return pasega;
    }
    void Stop(ASEGA* pasega)
    {
        if (!pasega) return;
        pasega->ieaEnd = int(PasegdOf(pasega->paseg)->aaev[2].size());   // (no end events)
        g_fAsegaEventsMuted = true;
        RetractAsega(pasega);
        g_fAsegaEventsMuted = false;
        m_apasega.erase(std::remove(m_apasega.begin(), m_apasega.end(), pasega), m_apasega.end());
    }
    // one game step at the clip's time t (dt: the step, the chains' simulation)
    void Step(ASEGA* pasega, float t, float dt)
    {
        m_tClock += dt;
        g_clock.t = m_tClock;
        g_clock.dt = dt;
        if (pasega) pasega->t = t;
        for (ALO* palo : m_apalo) UpdateAloXfs(palo, g_clock.t, dt);
    }
    const std::vector<ALO*>& Apalo() const { return m_apalo; }

private:
    struct XFSAVE
    {
        XF* pxf;
        XF xf;
        std::vector<CTRL*> actrl;
    };
    struct ALOSAVE
    {
        ALO* palo = nullptr;
        uint32_t grfUpd = 0;
        std::vector<ALOPAIR> apair;
        std::vector<std::pair<float, glm::vec3>> achain;
        std::vector<std::pair<glm::vec3, glm::vec3>> achaine;
    };
    struct TREESAVE
    {
        std::vector<XFSAVE> axf;
        std::vector<ALOSAVE> aalo;
    };
    static XFSAVE SaveXf(XF* pxf)
    {
        XFSAVE s{pxf, *pxf, {}};
        for (CTRL* q = pxf->pctrlFirst; q; q = q->pnext) s.actrl.push_back(q);
        return s;
    }
    void SaveTree(TREESAVE& ts)
    {
        ts = TREESAVE();
        for (ALO* palo : m_apalo)
        {
            ALOSAVE as;
            as.palo = palo;
            as.grfUpd = GrfUpdOfLo(palo);
            as.apair = palo->apair;
            for (const ALOCHAIN& c : palo->achain)
            {
                as.achain.push_back({c.tLast, c.posAnchor});
                for (const ALOCHAINE& e : c.ae) as.achaine.push_back({e.pos, e.v});
            }
            ts.aalo.push_back(std::move(as));
            for (auto& up : palo->axf)
            {
                ts.axf.push_back(SaveXf(up.get()));
                m_setxf.insert(up.get());
            }
        }
    }
    static void RestoreTree(const TREESAVE& ts)
    {
        for (auto it = ts.axf.rbegin(); it != ts.axf.rend(); ++it)
        {
            *it->pxf = it->xf;
            Relink(it->pxf, it->actrl);
        }
        for (const ALOSAVE& as : ts.aalo)
        {
            ALO* palo = as.palo;
            for (std::size_t i = 0; i < palo->apair.size() && i < as.apair.size(); ++i) palo->apair[i] = as.apair[i];
            std::size_t ic = 0, ie = 0;
            for (ALOCHAIN& c : palo->achain)
            {
                if (ic >= as.achain.size()) break;
                c.tLast = as.achain[ic].first;
                c.posAnchor = as.achain[ic++].second;
                for (ALOCHAINE& e : c.ae)
                    if (ie < as.achaine.size()) { e.pos = as.achaine[ie].first; e.v = as.achaine[ie++].second; }
            }
            SetGrfUpdOfLo(palo, as.grfUpd);
        }
    }
    static void Relink(XF* pxf, const std::vector<CTRL*>& a)
    {
        pxf->pctrlFirst = a.empty() ? nullptr : a[0];
        for (std::size_t i = 0; i < a.size(); ++i) a[i]->pnext = i + 1 < a.size() ? a[i + 1] : nullptr;
    }
    ALO* m_proot;
    SW* m_pswSaved = nullptr;
    std::vector<ALO*> m_apalo;
    TREESAVE m_orig, m_start;
    std::set<const XF*> m_setxf;
    std::vector<CTRL*> m_actrlMade;
    std::vector<ASEGA*> m_apasega;
    CLOCK m_clock;
    float m_tClock = 10000.0f;   // (the chains simulate when the clock's time changes)
};

// The records' worlds the skin is bound in (bone.cpp BindAloBones: at the post-load, before any animation; the
// editor's world is never animated, so its records' worlds now)
glm::mat4 MatBindWorld(const XF* pxf) { return pxf ? pxf->matWorld : glm::mat4(1.0f); }

// A transform's translation / rotation / scale (a mirrored frame: the x scale negative)
void DecomposeTrs(const glm::mat4& m, glm::vec3& t, glm::quat& q, glm::vec3& s)
{
    t = glm::vec3(m[3]);
    s = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    if (glm::determinant(glm::mat3(m)) < 0) s.x = -s.x;
    const glm::mat3 r(glm::vec3(m[0]) / s.x, glm::vec3(m[1]) / s.y, glm::vec3(m[2]) / s.z);
    q = glm::normalize(glm::quat_cast(r));
}
glm::mat4 MatTrs(const glm::vec3& t, const glm::quat& q, const glm::vec3& s)
{
    glm::mat4 m = glm::mat4_cast(q);
    m[0] *= s.x;
    m[1] *= s.y;
    m[2] *= s.z;
    m[3] = glm::vec4(t, 1.0f);
    return m;
}

// The rotation and translation of a transform (polar decomposition: the nearest rotation to its 3x3). glTF's joints
// inherit their parents' whole transform: a record's small squash / stretch in the game (its non-uniform scale) under
// rotated children would need a shear, which a joint's translation / rotation / scale cannot hold, so the baked joints
// are rigid (the skinned surface differs from the game's by that squash only, see FPoseDump's check).
glm::mat4 MatRigid(const glm::mat4& m)
{
    glm::mat3 r(m);
    if (glm::determinant(r) < 0) r[0] = -r[0];
    for (int i = 0; i < 20; ++i)
    {
        const glm::mat3 n = 0.5f * (r + glm::transpose(glm::inverse(r)));
        const bool fDone = glm::length(n[0] - r[0]) + glm::length(n[1] - r[1]) + glm::length(n[2] - r[2]) < 1e-7f;
        r = n;
        if (fDone) break;
    }
    glm::mat4 o(r);
    o[3] = m[3];
    return o;
}

// A symmetric 3x3 matrix's eigenvectors (the columns of the rotation Q) and values (L): K = Q diag(L) Q^T (Jacobi).
// The columns are ordered and signed to follow Qprev's (a squash's axes change little from key to key).
void EigenSym3(const glm::mat3& K, const glm::mat3& Qprev, glm::mat3& Q, glm::vec3& L)
{
    double a[3][3], v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) a[i][j] = K[j][i];
    for (int sweep = 0; sweep < 32; ++sweep)
    {
        const double off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
        if (off < 1e-12) break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q)
            {
                if (std::fabs(a[p][q]) < 1e-15) continue;
                const double th = 0.5 * std::atan2(2.0 * a[p][q], a[q][q] - a[p][p]);
                const double c = std::cos(th), s = std::sin(th);
                for (int k = 0; k < 3; ++k)
                {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    glm::vec3 ae[3];
    for (int c = 0; c < 3; ++c) ae[c] = glm::vec3(float(v[0][c]), float(v[1][c]), float(v[2][c]));
    // the order and signs nearest the previous axes
    int aperm[3] = {0, 1, 2}, abest[3] = {0, 1, 2};
    float best = -1.0f;
    do
    {
        float sum = 0.0f;
        for (int c = 0; c < 3; ++c) sum += std::fabs(glm::dot(ae[aperm[c]], Qprev[c]));
        if (sum > best) { best = sum; std::copy(aperm, aperm + 3, abest); }
    } while (std::next_permutation(aperm, aperm + 3));
    for (int c = 0; c < 3; ++c)
    {
        glm::vec3 e = ae[abest[c]];
        if (glm::dot(e, Qprev[c]) < 0) e = -e;
        Q[c] = e;
        L[c] = float(a[abest[c]][abest[c]]);
    }
    if (glm::determinant(Q) < 0)
    {
        int cMin = 0;
        for (int c = 1; c < 3; ++c) if (std::fabs(glm::dot(Q[c], Qprev[c])) < std::fabs(glm::dot(Q[cMin], Qprev[cMin]))) cMin = c;
        Q[cMin] = -Q[cMin];
    }
}

// The angle between two unit quaternions (x y z w), precise for small ones (an acos of their dot is not, in floats)
float QuatAngle(const glm::vec4& a, const glm::vec4& b)
{
    const float d = std::min(glm::length(a - b), glm::length(a + b));
    return 4.0f * std::asin(std::min(1.0f, 0.5f * d));
}

// A channel's value at time t (linear; rotations spherical), as a glTF viewer interpolates it
glm::vec4 SampleChannel(const md::Channel& ch, float t)
{
    if (ch.t.size() == 1 || t <= ch.t.front()) return ch.v.front();
    if (t >= ch.t.back()) return ch.v.back();
    std::size_t i = 1;
    while (ch.t[i] < t) ++i;
    const float u = (t - ch.t[i - 1]) / std::max(ch.t[i] - ch.t[i - 1], 1e-9f);
    if (ch.path != 1) return ch.v[i - 1] + (ch.v[i] - ch.v[i - 1]) * u;
    const glm::quat a(ch.v[i - 1].w, ch.v[i - 1].x, ch.v[i - 1].y, ch.v[i - 1].z), b(ch.v[i].w, ch.v[i].x, ch.v[i].y, ch.v[i].z);
    const glm::quat q = glm::slerp(a, b, u);
    return glm::vec4(q.x, q.y, q.z, q.w);
}

// The keys a channel needs (mask): dropped where the interpolation between the kept neighbours stays within tol
// (translation / scale: units; rotation: radians); the first and last stay; a channel that never moves keeps one
std::vector<bool> MaskReduceKeys(const md::Channel& ch, float tol)
{
    const std::size_t n = ch.t.size();
    std::vector<bool> mask(n, n < 3);
    if (n < 3) return mask;
    auto err = [&](std::size_t a, std::size_t b, std::size_t k) {
        const float u = (ch.t[k] - ch.t[a]) / std::max(ch.t[b] - ch.t[a], 1e-9f);
        if (ch.path == 1)
        {
            glm::quat qa(ch.v[a].w, ch.v[a].x, ch.v[a].y, ch.v[a].z), qb(ch.v[b].w, ch.v[b].x, ch.v[b].y, ch.v[b].z), qk(ch.v[k].w, ch.v[k].x, ch.v[k].y, ch.v[k].z);
            const glm::quat qi = glm::slerp(qa, qb, u);
            return QuatAngle(glm::vec4(qi.x, qi.y, qi.z, qi.w), glm::vec4(qk.x, qk.y, qk.z, qk.w));
        }
        return glm::length(glm::vec3(ch.v[a] + (ch.v[b] - ch.v[a]) * u - ch.v[k]));
    };
    mask[0] = true;
    std::size_t a = 0;
    for (std::size_t b = 2; b < n; ++b)
    {
        bool fOk = true;
        for (std::size_t k = a + 1; k < b && fOk; ++k) fOk = err(a, b, k) <= tol;
        if (!fOk) { a = b - 1; mask[a] = true; }
    }
    mask[n - 1] = true;
    bool fConst = true;
    for (std::size_t k = 1; k < n && fConst; ++k) fConst = err(0, 0, k) <= tol;
    if (fConst) std::fill(mask.begin() + 1, mask.end(), false);
    return mask;
}

md::Channel ChannelMasked(const md::Channel& ch, const std::vector<bool>& mask)
{
    md::Channel o;
    o.joint = ch.joint;
    o.path = ch.path;
    for (std::size_t k = 0; k < ch.t.size(); ++k)
        if (mask[k]) { o.t.push_back(ch.t[k]); o.v.push_back(ch.v[k]); }
    return o;
}

// A character's skeleton made readable (the joints are sorted, parents set): each joint's bind frame turned into a
// right-handed one (+Y along the bone toward its child, +X a limb joint's bend axis; skinning does not change: the
// inverse binds follow), the limbs' records named by their part, the rest poses and inverse binds from those frames.
// apxf / amatFrame: per joint its record and the frame change (the record's world x it = the joint's frame).
void BuildRig(ALO* palo, md::Scene& sc, const std::map<const XF*, int>& mpjoint, const glm::mat4& C, const glm::mat4& refBind, bool fRaw,
              std::vector<const XF*>& apxf, std::vector<glm::mat4>& amatFrame)
{
    const std::size_t cj = sc.joints.size();
    const glm::mat4 Cinv = glm::inverse(C);
    apxf.assign(cj, nullptr);
    for (const auto& [pxf, ij] : mpjoint) apxf[size_t(ij)] = pxf;
    std::vector<glm::mat4> aB(cj);
    for (std::size_t j = 0; j < cj; ++j) aB[j] = C * refBind * MatBindWorld(apxf[j]) * Cinv;
    auto jointOf = [&](const XF* pxf) { const auto it = mpjoint.find(pxf); return it == mpjoint.end() ? -1 : it->second; };
    // the limbs (the two-bone joints): legs / arms by their end's height, sides by its y (the character's frame: x ahead,
    // y left, z up)
    struct ROLE { std::string sz; glm::vec3 posNext{0.0f}; bool fNext = false; glm::vec3 axis{0.0f}; bool fAxis = false; };
    std::vector<ROLE> arole(cj);
    std::vector<int> aiq;
    float zTop = 0.0f;
    for (auto& up : palo->axf) zTop = std::max(zTop, (refBind * MatBindWorld(up.get()))[3].z);
    for (std::size_t iq = 0; iq < palo->apair.size(); ++iq)
        if (palo->apair[iq].apxf[1] && palo->apair[iq].apxf[2] && palo->apair[iq].apxf[3]) aiq.push_back(int(iq));
    auto posOf = [&](const XF* pxf) { return glm::vec3((refBind * MatBindWorld(pxf))[3]); };
    // (the limb's end point: the end record's point, ik.cpp's vecB0; the export's space)
    auto posEndOf = [&](const ALOPAIR& q) { return glm::vec3(C * refBind * MatBindWorld(q.apxf[3]) * glm::vec4(q.vecB0, 1.0f)); };
    std::stable_sort(aiq.begin(), aiq.end(), [&](int a, int b) { return posOf(palo->apair[size_t(a)].apxf[3]).z < posOf(palo->apair[size_t(b)].apxf[3]).z; });
    std::map<std::string, int> mpcount;
    nlohmann::json aik = nlohmann::json::array();
    for (std::size_t k = 0; k < aiq.size(); ++k)
    {
        const ALOPAIR& q = palo->apair[size_t(aiq[k])];
        const glm::vec3 posEnd = posOf(q.apxf[3]);
        std::string limb = aiq.size() == 4 ? (k < 2 ? "leg" : "arm") : aiq.size() == 2 ? (posEnd.z < 0.35f * zTop ? "leg" : "arm") : "limb" + std::to_string(k);
        std::string side = posEnd.y >= 0.0f ? "L" : "R";
        if (mpcount[limb + side]++) side += std::to_string(mpcount[limb + side]);
        const bool fLeg = limb == "leg", fArm = limb == "arm";
        // the parts: the target (the bone the clip moves: the foot / hand), the upper and lower bones the joint solves,
        // and the end (the record holding the point that reaches the target, when not the lower bone itself)
        const char* aszPart[4] = {fLeg ? "foot" : fArm ? "hand" : "target", fLeg ? "thigh" : fArm ? "upperarm" : "upper",
                                  fLeg ? "shin" : fArm ? "forearm" : "lower", fLeg ? "ankle" : fArm ? "wrist" : "end"};
        // the bend axis from the bind pose (ik.cpp InitAloPairRest's: upper -> lower x lower -> the end point); a
        // straight limb: the character's sideways axis
        const glm::vec3 pU = glm::vec3(C * glm::vec4(posOf(q.apxf[1]), 1.0f)), pL = glm::vec3(C * glm::vec4(posOf(q.apxf[2]), 1.0f)), pE = posEndOf(q);
        glm::vec3 axis = glm::cross(pL - pU, pE - pL);
        if (glm::length(axis) < 0.03f * glm::length(pL - pU) * glm::length(pE - pL)) axis = glm::vec3(glm::mat3(C) * glm::vec3(0.0f, 1.0f, 0.0f));
        axis = glm::normalize(axis);
        nlohmann::json e;
        for (int r = 0; r < 4; ++r)
        {
            const int j = jointOf(q.apxf[r]);
            if (j < 0 || (r == 3 && q.apxf[3] == q.apxf[2])) continue;
            arole[size_t(j)].sz = std::string(aszPart[r]) + "_" + side;
            if (r == 1 || r == 2)
            {
                arole[size_t(j)].posNext = r == 1 ? pL : pE;
                arole[size_t(j)].fNext = true;
                arole[size_t(j)].axis = axis;
                arole[size_t(j)].fAxis = true;
            }
            e[r == 0 ? "target" : r == 1 ? "upper" : r == 2 ? "lower" : "end"] = sc.joints[size_t(j)].name + "_" + arole[size_t(j)].sz;
        }
        aik.push_back(e);
    }
    for (std::size_t j = 0; j < cj; ++j)
        if (!arole[j].sz.empty()) sc.joints[j].name += "_" + arole[j].sz;
    // the frames
    std::vector<std::vector<int>> achild(cj);
    for (std::size_t j = 0; j < cj; ++j) if (sc.joints[j].parent >= 0) achild[size_t(sc.joints[j].parent)].push_back(int(j));
    std::vector<glm::mat4> aF(cj, glm::mat4(1.0f));
    amatFrame.assign(cj, glm::mat4(1.0f));
    for (std::size_t j = 0; j < cj; ++j)
    {
        if (sc.joints[j].parent < 0) { aF[j] = glm::mat4(1.0f); amatFrame[j] = glm::inverse(aB[j]); continue; }   // (the root: the placement)
        const glm::vec3 pos(aB[j][3]);
        glm::vec3 y(0.0f);
        auto dirTo = [&](const glm::vec3& p) { const glm::vec3 d = p - pos; return glm::length(d) > 1e-5f ? glm::normalize(d) : glm::vec3(0.0f); };
        if (arole[j].fNext) y = dirTo(arole[j].posNext);
        if (y == glm::vec3(0.0f) && !achild[j].empty())
        {
            glm::vec3 avg(0.0f);
            for (int c : achild[j]) avg += glm::vec3(aB[size_t(c)][3]);
            y = dirTo(avg / float(achild[j].size()));
            float dMax = 0.0f;
            if (y == glm::vec3(0.0f))
                for (int c : achild[j])
                    if (const float d = glm::length(glm::vec3(aB[size_t(c)][3]) - pos); d > dMax) { dMax = d; y = dirTo(glm::vec3(aB[size_t(c)][3])); }
        }
        // (a leaf: along the line from its parent, the first ancestor apart from it)
        for (int p = sc.joints[j].parent; y == glm::vec3(0.0f) && p >= 0; p = sc.joints[size_t(p)].parent) y = -dirTo(glm::vec3(aB[size_t(p)][3]));
        if (y == glm::vec3(0.0f)) y = glm::normalize(glm::vec3(aB[j][1]));
        glm::vec3 x;
        if (arole[j].fAxis && std::fabs(glm::dot(arole[j].axis, y)) < 0.95f) x = arole[j].axis;
        else
        {
            float dMin = 2.0f;
            for (int c = 0; c < 3; ++c)
            {
                const glm::vec3 a = glm::normalize(glm::vec3(aB[j][c]));
                if (std::fabs(glm::dot(a, y)) < dMin) { dMin = std::fabs(glm::dot(a, y)); x = a; }
            }
        }
        x = glm::normalize(x - y * glm::dot(x, y));
        const glm::vec3 z = glm::cross(x, y);
        aF[j] = glm::mat4(glm::vec4(x, 0.0f), glm::vec4(y, 0.0f), glm::vec4(z, 0.0f), glm::vec4(pos, 1.0f));
        amatFrame[j] = glm::inverse(aB[j]) * aF[j];
    }
    for (std::size_t j = 0; j < cj; ++j)
    {
        const int ip = sc.joints[j].parent;
        sc.joints[j].local = ip >= 0 ? glm::inverse(aF[size_t(ip)]) * aF[j] : aF[j];
        sc.joints[j].inverseBind = glm::inverse(aF[j]);
    }
    // what the file's animations are, on the root joint (Blender: its custom properties)
    for (std::size_t j = 0; j < cj; ++j)
        if (sc.joints[j].parent < 0)
        {
            nlohmann::json e;
            e["safehouse_animations"] = fRaw ? "clip channels only: the limbs are not solved (the joints in safehouse_ik: rebuild them with IK constraints)"
                                             : "as the game shows them: every joint baked per frame (limbs solved, links, chains)";
            if (!aik.empty()) e["safehouse_ik"] = aik;
            sc.joints[j].extras = e.dump();
        }
}
}  // namespace

// The skeleton of a character's export: per joint its record, its bind world (the export's space, before the frame
// change) and the frame change (bind x frame: a right-handed frame, +Y along the bone toward its child, +X the bend
// axis of a limb's joints)
struct CEditor::EXPORTRIG
{
    std::vector<const XF*> apxf;     // sc.joints order
    std::vector<glm::mat4> amatFrame;
    glm::mat4 C{1.0f};               // game -> glTF
    bool fSolve = true;
};

void CEditor::ExportAnimations(LO* plo, md::Scene& sc, const EXPORTRIG& rig)
{
    ALO* palo = dynamic_cast<ALO*>(plo);
    if (!palo || rig.apxf.empty() || !m_psw) return;
    std::vector<ASEG*> apaseg;
    std::function<void(LO*)> collect = [&](LO* p) {
        if (FCidDerivedFrom(p->Cid(), 0x6D) && std::find(apaseg.begin(), apaseg.end(), p) == apaseg.end()) apaseg.push_back(static_cast<ASEG*>(p));
        if (const auto it = m_mpchildren.find(p); it != m_mpchildren.end())
            for (LO* c : it->second) collect(c);
    };
    collect(plo);
    const glm::mat4 C = rig.C, Cinv = glm::inverse(C);
    const std::size_t cj = rig.apxf.size();
    const int fps = m_exportFps >= 60 ? 60 : 30;
    const float dtStep = 1.0f / 60.0f;   // (the game's step)
    const int cstepKey = 60 / fps;
    const float unit = 100.0f / (m_importScale > 0 ? m_importScale : 100.0f);   // (1 m in the file's units)
    // the export's space: the character's placement record as it stands (the skeleton's root joint)
    const glm::mat4 refInv = glm::inverse(MatWorldOfAlo(palo));
    POSER poser(m_psw.get(), palo, apaseg, rig.fSolve);
    // each clip played from its start, settled first (the chains hang, the joints' targets in place; a cycle (its end
    // pose its start's) played twice before, so its chains swing as in a looping play); fn(t) after each step that is a
    // key's
    auto play = [&](ASEG* paseg, bool fCycle, const std::function<void(float)>& fn) {
        ASEGD* pd = PasegdOf(paseg);
        const float tMax = std::clamp(pd->tMax, 0.0f, 120.0f);
        ASEGA* pasega = poser.Play(paseg);
        if (!pasega) return false;
        const int cstep = std::max(0, int(std::ceil(tMax / dtStep - 1e-3f)));
        for (int i = 0; i < 30; ++i) poser.Step(pasega, 0.0f, dtStep);
        if (fCycle)
            for (int i = 0; i < 2 * cstep; ++i) poser.Step(pasega, float(i % cstep) * dtStep, dtStep);
        for (int i = 0; i <= cstep; ++i)
        {
            const float t = std::min(float(i) * dtStep, tMax);
            poser.Step(pasega, t, dtStep);
            if (i % cstepKey == 0 || i == cstep) fn(t);
        }
        poser.Stop(pasega);
        return true;
    };
    auto worldOf = [&](std::size_t j) { return C * refInv * rig.apxf[j]->matWorld * Cinv * rig.amatFrame[j]; };
    std::vector<ASEG*> apasegAnim;
    for (ASEG* paseg : apaseg)
        if (ASEGD* pd = PasegdOf(paseg); pd && !pd->achn.empty() && apasegAnim.size() < 1000 &&
            std::find(apasegAnim.begin(), apasegAnim.end(), paseg) == apasegAnim.end())
            apasegAnim.push_back(paseg);
    // 1. the joints the game squashes / shears somewhere (a record's non-uniform scale, inherited by rotated children):
    // a joint's translation / rotation / scale cannot hold a shear, nor can its children inherit a squash the game's do
    // not, so the joints stay rigid and such a joint gets two more under it that carry it, and its skin moves to them:
    // "<joint>_deform_scale" (rotation Q, scale L) and "<joint>_deform" (rotation Q^T): the joint x Q L Q^T is the game's
    // matrix (Q L Q^T: the symmetric part of the polar decomposition)
    // (and the cycles: the clips whose joints end where they start, chains aside)
    std::vector<float> adev(cj, 0.0f);
    std::set<const XF*> setxfChain;
    for (ALO* pa : poser.Apalo())
        for (const ALOCHAIN& c : pa->achain)
            for (const ALOCHAINE& e : c.ae)
                if (e.pxf) setxfChain.insert(e.pxf);
    auto fChained = [&](const XF* pxf) {
        for (const XF* p = pxf; p; p = p->pxfRef) if (setxfChain.count(p)) return true;
        return false;
    };
    std::vector<bool> afCycle(apasegAnim.size(), false);
    for (std::size_t ia = 0; ia < apasegAnim.size(); ++ia)
    {
        std::vector<glm::vec3> apos0, apos1;
        float dRange = 0.0f;
        play(apasegAnim[ia], false, [&](float) {
            apos1.clear();
            for (std::size_t j = 0; j < cj; ++j)
            {
                const glm::mat4 w = worldOf(j);
                const glm::mat3 K = glm::transpose(glm::mat3(MatRigid(w))) * glm::mat3(w);
                for (int c = 0; c < 3; ++c) adev[j] = std::max(adev[j], glm::length(K[c] - glm::mat3(1.0f)[c]));
                if (!fChained(rig.apxf[j])) apos1.push_back(glm::vec3(w[3]));
            }
            if (apos0.empty()) apos0 = apos1;
            for (std::size_t k = 0; k < apos0.size() && k < apos1.size(); ++k) dRange = std::max(dRange, glm::length(apos0[k] - apos1[k]));
        });
        float d = apos0.size() == apos1.size() && apos0.size() > 0 ? 0.0f : 1e9f;
        for (std::size_t k = 0; k < apos0.size() && k < apos1.size(); ++k) d = std::max(d, glm::length(apos0[k] - apos1[k]));
        // (a cycle: it ends near its start, against how far its joints go (a run's end is its next start's frame before))
        afCycle[ia] = d < std::max(0.02f * unit, 0.25f * dRange) && PasegdOf(apasegAnim[ia])->tMax > 0.1f;
        if (std::getenv("SLY2N_EDITOR_EXPORTDBG"))
            std::printf("export animations: clip %d %.3f s: start / end apart by %.4f of %.4f%s\n", apasegAnim[ia]->oid, PasegdOf(apasegAnim[ia])->tMax, d, dRange, afCycle[ia] ? " (a cycle)" : "");
    }
    std::vector<int> ajointDeform(cj, -1);   // the joint's "_deform_scale" (its "_deform": the next one)
    for (std::size_t j = 0; j < cj; ++j)
    {
        if (adev[j] < 0.004f || sc.joints[j].parent < 0) continue;
        ajointDeform[j] = int(sc.joints.size());
        md::Joint a, c;
        a.name = sc.joints[j].name + "_deform_scale";
        a.parent = int(j);
        a.inverseBind = sc.joints[j].inverseBind;
        c.name = sc.joints[j].name + "_deform";
        c.parent = ajointDeform[j];
        c.inverseBind = sc.joints[j].inverseBind;
        sc.joints.push_back(a);
        sc.joints.push_back(c);
    }
    for (md::Mesh& mesh : sc.meshes)
        for (md::Primitive& p : mesh.prims)
            for (std::size_t iv = 0; iv < p.joints.size(); ++iv)
                for (int q = 0; q < 4; ++q)
                    if (p.weights[iv][q] > 0 && ajointDeform[p.joints[iv][q]] >= 0) p.joints[iv][q] = uint16_t(ajointDeform[p.joints[iv][q]] + 1);
    const std::size_t cjAll = sc.joints.size();
    // each joint's reach: its skin's farthest vertex at the bind (a rotation's error moves it by this much)
    std::vector<float> areach(cjAll, 0.1f * unit);
    for (const md::Mesh& mesh : sc.meshes)
        for (const md::Primitive& p : mesh.prims)
            for (std::size_t iv = 0; iv < p.joints.size(); ++iv)
                for (int q = 0; q < 4; ++q)
                    if (p.weights[iv][q] > 0)
                    {
                        const int j = p.joints[iv][q];
                        areach[size_t(j)] = std::max(areach[size_t(j)], glm::length(p.pos[iv] - glm::vec3(glm::inverse(sc.joints[size_t(j)].inverseBind)[3])));
                    }
    // 2. the clips' keys: every joint's world (rigid; the deform joints: the game's matrix) per key, its local
    // translation / rotation / scale from its parent's, fewer keys where the error stays within tolErr
    const float tolErr = 0.0005f * unit;   // (0.5 mm)
    double errMax = 0.0;
    std::size_t ckey = 0;
    for (std::size_t ia = 0; ia < apasegAnim.size(); ++ia)
    {
        ASEG* paseg = apasegAnim[ia];
        std::vector<float> at;
        std::vector<std::vector<glm::mat4>> aamat;   // per key, per joint: its world (the export's space, the joint's frame)
        std::vector<glm::mat3> aQ(cjAll, glm::mat3(1.0f));   // (the deform rotations of the previous key: continuity)
        if (!play(paseg, afCycle[ia], [&](float t) {
                at.push_back(t);
                std::vector<glm::mat4> amat(cjAll);
                for (std::size_t j = 0; j < cj; ++j)
                {
                    const glm::mat4 w = worldOf(j);
                    amat[j] = MatRigid(w);
                    if (const int a = ajointDeform[j]; a >= 0)
                    {
                        const glm::mat3 K = glm::transpose(glm::mat3(amat[j])) * glm::mat3(w);
                        glm::mat3 Q;
                        glm::vec3 L;
                        EigenSym3(0.5f * (K + glm::transpose(K)), aQ[size_t(a)], Q, L);
                        // (no squash: the axes are any, the last ones kept so the keys stay still)
                        if (glm::all(glm::lessThan(glm::abs(L - glm::vec3(1.0f)), glm::vec3(2e-4f)))) { Q = aQ[size_t(a)]; L = glm::vec3(1.0f); }
                        aQ[size_t(a)] = Q;
                        glm::mat4 wa = amat[j] * glm::mat4(Q * glm::mat3(glm::scale(glm::mat4(1.0f), L)));
                        wa[3] = amat[j][3];
                        amat[size_t(a)] = wa;
                        glm::mat4 wc = amat[j] * glm::mat4(Q * glm::mat3(glm::scale(glm::mat4(1.0f), L)) * glm::transpose(Q));
                        wc[3] = amat[j][3];
                        amat[size_t(a) + 1] = wc;
                    }
                }
                aamat.push_back(std::move(amat));
            }))
            continue;
        md::Animation an;
        an.name = "aseg_" + std::to_string(paseg->oid);
        bool fMoves = false;
        for (std::size_t j = 0; j < cjAll; ++j)
        {
            md::Channel T, R, S;
            T.joint = R.joint = S.joint = int(j);
            T.path = 0; R.path = 1; S.path = 2;
            glm::quat qPrev(1, 0, 0, 0);
            const int ip = sc.joints[j].parent;
            for (std::size_t k = 0; k < at.size(); ++k)
            {
                const glm::mat4 local = ip >= 0 ? glm::inverse(aamat[k][size_t(ip)]) * aamat[k][j] : aamat[k][j];
                glm::vec3 t, s;
                glm::quat q;
                DecomposeTrs(local, t, q, s);
                if (glm::dot(q, qPrev) < 0) q = -q;
                qPrev = q;
                T.t.push_back(at[k]); R.t.push_back(at[k]); S.t.push_back(at[k]);
                T.v.push_back(glm::vec4(t, 0.0f));
                R.v.push_back(glm::vec4(q.x, q.y, q.z, q.w));
                S.v.push_back(glm::vec4(s, 0.0f));
                if (k > 0 && (glm::length(T.v[k] - T.v[0]) > 1e-5f || glm::length(R.v[k] - R.v[0]) > 1e-5f)) fMoves = true;
            }
            an.ch.push_back(std::move(T));
            an.ch.push_back(std::move(R));
            an.ch.push_back(std::move(S));
        }
        if (!fMoves) continue;
        // fewer keys: each channel reduced, then keys put back (for the joint and the ones above it) wherever a joint's
        // transform from its parents' keys, as a viewer interpolates them, moves its skin by more than tolErr
        std::vector<std::vector<bool>> amask(an.ch.size());
        for (std::size_t c = 0; c < an.ch.size(); ++c)
            amask[c] = m_exportReduce ? MaskReduceKeys(an.ch[c], an.ch[c].path == 1 ? 0.25f * tolErr / areach[c / 3] : 0.25f * tolErr)
                                      : std::vector<bool>(an.ch[c].t.size(), true);
        double errClip = 0.0;
        for (int pass = 0; pass < 64; ++pass)
        {
            std::vector<md::Channel> ach(an.ch.size());
            for (std::size_t c = 0; c < an.ch.size(); ++c) ach[c] = ChannelMasked(an.ch[c], amask[c]);
            bool fAdded = false;
            errClip = 0.0;
            for (std::size_t k = 0; k < at.size(); ++k)
            {
                std::vector<glm::mat4> aw(cjAll);
                for (std::size_t j = 0; j < cjAll; ++j)
                {
                    const glm::vec4 t = SampleChannel(ach[3 * j], at[k]), r = SampleChannel(ach[3 * j + 1], at[k]), s = SampleChannel(ach[3 * j + 2], at[k]);
                    const glm::mat4 l = MatTrs(glm::vec3(t), glm::quat(r.w, r.x, r.y, r.z), glm::vec3(s));
                    aw[j] = sc.joints[j].parent >= 0 ? aw[size_t(sc.joints[j].parent)] * l : l;
                    double e = glm::length(glm::vec3(aw[j][3] - aamat[k][j][3]));
                    for (int c = 0; c < 3; ++c) e = std::max(e, double(glm::length(glm::vec3(aw[j][c] - aamat[k][j][c])) * areach[j]));
                    errClip = std::max(errClip, e);
                    // (the key back in the joint's own channels that miss their value there, else in the nearest joint's
                    // above it)
                    if (e > tolErr && pass < 63)
                        for (int p = int(j); p >= 0; p = sc.joints[size_t(p)].parent)
                        {
                            bool fAny = false;
                            for (int c = 0; c < 3; ++c)
                            {
                                const std::size_t ic = size_t(3 * p + c);
                                if (amask[ic][k]) continue;
                                const glm::vec4 v = SampleChannel(ach[ic], at[k]), w = an.ch[ic].v[k];
                                const float d = an.ch[ic].path == 1 ? QuatAngle(v, w) : glm::length(glm::vec3(v - w));
                                if (d > 1e-6f) { amask[ic][k] = true; fAny = true; }
                            }
                            if (fAny) { fAdded = true; break; }
                        }
                }
            }
            if (!fAdded || pass == 63) { an.ch = std::move(ach); break; }
        }
        errMax = std::max(errMax, errClip);
        for (const md::Channel& ch : an.ch) ckey += ch.t.size();
        sc.anims.push_back(std::move(an));
    }
    int cdeform = 0;
    for (int a : ajointDeform) cdeform += a >= 0;
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.2f mm", errMax * 1000.0 / unit);
    m_exportAnimInfo = std::to_string(sc.anims.size()) + " of " + std::to_string(apaseg.size()) + " clips " + (rig.fSolve ? "as in the game" : "(clip channels only)") +
                       ", " + std::to_string(ckey) + " keys, largest error " + sz + (cdeform ? ", " + std::to_string(cdeform) + " joints with deform bones" : "");
    if (std::getenv("SLY2N_EDITOR_EXPORTDBG"))
    {
        std::printf("export animations: %d keys a second; %s\n", fps, m_exportAnimInfo.c_str());
        for (std::size_t j = 0; j < cj; ++j)
            if (ajointDeform[j] >= 0) std::printf("export animations: %s squashed / sheared by up to %.3f\n", sc.joints[j].name.c_str(), adev[j]);
    }
}

// The renderer's skinned vertices of the character at clip oidAseg's time t (-1: no clip), posed by the game's update
// as the export bakes it and skinned by the renderer's own code (bone.cpp: the bindings made for it here, dropped
// after), in the export's space (glTF: Y up, the file's units): per drawn group "glob NAME" then "x y z" lines in the
// export's vertex order, and "xf OID RECORD x y z" per record. A check of an export (build/: gltfeval.py, Blender).
bool CEditor::FPoseDump(LO* plo, int oidAseg, float t, bool fCycle, const std::string& path, std::string& error)
{
    ALO* palo = dynamic_cast<ALO*>(plo);
    if (!palo || !palo->pglobset || !m_psw) { error = "select a character"; return false; }
    ASEG* paseg = nullptr;
    std::function<void(LO*)> find = [&](LO* p) {
        if (FCidDerivedFrom(p->Cid(), 0x6D) && p->oid == oidAseg) paseg = static_cast<ASEG*>(p);
        if (const auto it = m_mpchildren.find(p); it != m_mpchildren.end())
            for (LO* c : it->second) find(c);
    };
    find(plo);
    if (oidAseg >= 0 && !paseg) { error = "no clip " + std::to_string(oidAseg) + " under the character"; return false; }
    std::FILE* pf = std::fopen(path.c_str(), "w");
    if (!pf) { error = "cannot write " + path; return false; }
    const float scale = m_importScale > 0 ? m_importScale : 100.0f;
    const glm::mat4 C = glm::scale(glm::mat4(glm::transpose(AxisToGame())), glm::vec3(1.0f / scale));
    const glm::mat4 refInv = glm::inverse(MatWorldOfAlo(palo));
    BindAloBones(palo);   // (the bindings at the editor's (never animated) pose, as the game's post-load makes them)
    {
        POSER poser(m_psw.get(), palo, paseg ? std::vector<ASEG*>{paseg} : std::vector<ASEG*>{}, m_exportAnim == 0);
        ASEGA* pasega = paseg ? poser.Play(paseg) : nullptr;
        for (int i = 0; i < 30; ++i) poser.Step(pasega, 0.0f, 1.0f / 60.0f);
        // (fCycle: played twice through first, as the export plays a cycle)
        if (const int cstepClip = paseg ? int(std::ceil(PasegdOf(paseg)->tMax * 60.0f - 1e-3f)) : 0; fCycle && cstepClip > 0)
            for (int i = 0; i < 2 * cstepClip; ++i) poser.Step(pasega, float(i % cstepClip) / 60.0f, 1.0f / 60.0f);
        const int cstep = std::max(0, int(std::ceil(t * 60.0f - 1e-3f)));
        for (int i = 0; i <= cstep; ++i) poser.Step(pasega, std::min(float(i) / 60.0f, t), 1.0f / 60.0f);
        const GLOBSET& gs = *palo->pglobset;
        const auto* paabind = PaabindOfAlo(palo);
        for (std::size_t ig = 0; ig < gs.aglob.size(); ++ig)
        {
            const GLOB& glob = gs.aglob[ig];
            if (glob.meshk != MESHK_Strips) continue;
            const GLOB& geom = glob.iglobTemplate >= 0 && glob.avtx.empty() ? gs.aglob[size_t(glob.iglobTemplate)] : glob;
            // (the renderer's choice, scenesw.cpp: an instance with bones has its own binding)
            const int iglobBind = glob.iglobTemplate >= 0 && glob.abone.empty() ? glob.iglobTemplate : int(ig);
            std::vector<glm::mat4> amat;
            const bool fSkin = !geom.abone.empty() && FGlobBoneMatricesOf(paabind, iglobBind, &amat);
            const XF* pxfOwner = glob.oidOwner >= 0 ? PxfFindAlo(palo, glob.oidOwner) : nullptr;
            glm::mat4 m = pxfOwner ? pxfOwner->matWorld : MatWorldOfAlo(palo);
            if (glob.fMatrix) m = m * glob.dmat;
            std::fprintf(pf, "glob %s_%d_%zu\n", SzClassRaw(palo->Cid()), palo->oid, ig);
            // (the export's order: each part's vertices as its triangles first use them)
            std::vector<uint32_t> aiv;
            for (const SUBGLOB& sub : geom.asubglob)
            {
                std::set<uint32_t> setv;
                for (uint32_t k = 0; k < sub.indexCount; ++k)
                    if (setv.insert(geom.aindex[sub.firstIndex + k]).second) aiv.push_back(geom.aindex[sub.firstIndex + k]);
            }
            for (uint32_t iv : aiv)
            {
                const VTXGLOB& v = geom.avtx[iv];
                glm::vec4 p(0.0f);
                if (fSkin)
                {
                    // (light.cpp's sum: the weights as they are, a slot past the bones is the last one)
                    for (int q = 0; q < 4; ++q)
                        if (v.aw[q]) p += (float(v.aw[q]) / 255.0f) * (amat[std::min<size_t>(v.abone[q], amat.size() - 1)] * glm::vec4(v.pos, 1.0f));
                }
                else p = m * glm::vec4(v.pos, 1.0f);
                const glm::vec3 e = glm::vec3(C * refInv * glm::vec4(glm::vec3(p), 1.0f));
                std::fprintf(pf, "%.5f %.5f %.5f\n", e.x, e.y, e.z);
            }
        }
        for (std::size_t i = 0; i < palo->axf.size(); ++i)
        {
            const glm::vec3 e = glm::vec3(C * refInv * palo->axf[i]->matWorld[3]);
            std::fprintf(pf, "xf %d %zu %.5f %.5f %.5f\n", palo->oid, i, e.x, e.y, e.z);
        }
    }
    SetAloBoneBindings(palo, {});   // (unbound again: the editor draws it as it did)
    std::fclose(pf);
    Status("pose of clip " + std::to_string(oidAseg) + " at " + std::to_string(t) + " written to " + path);
    return true;
}

// A character (with the objects under it) posed by a clip at its time tClip as the game poses it (the cutscene editor's
// playhead): each of its records' world matrix relative to its object record (PxfObj: where the trace places it), the
// records' own state given back after. Its skin bound first (at the editor's pose, the bind pose) so the renderer
// skins it with the records' worlds the caller sets. paseg null: its rest pose (the joints solved).
bool CEditor::FPoseCharacter(LO* plo, LO* plAseg, float tClip, std::vector<std::pair<XF*, glm::mat4>>& arel)
{
    arel.clear();
    ALO* palo = dynamic_cast<ALO*>(plo);
    if (!palo || !m_psw) return false;
    ASEG* paseg = plAseg && FCidDerivedFrom(plAseg->Cid(), 0x6D) ? static_cast<ASEG*>(plAseg) : nullptr;
    // (the tree's skins bound once, at the pose they stand in now: the editor's world is never animated)
    for (const auto& up : m_psw->aploAll)
        if (FCidDerivedFrom(up->Cid(), 0x30) && FLoInParentChain(up.get(), palo))
        {
            ALO* p = static_cast<ALO*>(up.get());
            const auto* paab = PaabindOfAlo(p);
            if (p->pglobset && (!paab || paab->empty())) BindAloBones(p);
        }
    XF* pxfObj = PxfObj(palo) ? PxfObj(palo) : palo->axf.empty() ? nullptr : palo->axf[0].get();
    if (!pxfObj) return false;
    {
        POSER poser(m_psw.get(), palo, paseg ? std::vector<ASEG*>{paseg} : std::vector<ASEG*>{}, true);
        ASEGA* pasega = paseg ? poser.Play(paseg) : nullptr;
        const float t = paseg ? std::clamp(tClip, 0.0f, PasegdOf(paseg) ? PasegdOf(paseg)->tMax : tClip) : 0.0f;
        // (a few steps at that time: the joints and chains settle)
        for (int i = 0; i < 4; ++i) poser.Step(pasega, t, 1.0f / 60.0f);
        const glm::mat4 inv = glm::inverse(pxfObj->matWorld);
        for (ALO* p : poser.Apalo())
            for (auto& up : p->axf) arel.push_back({up.get(), inv * up->matWorld});
    }
    return !arel.empty();
}

// The bindings FPoseCharacter made taken away again (the editor draws the character unskinned, as it did)
void CEditor::UnbindPosed(LO* plo)
{
    ALO* palo = dynamic_cast<ALO*>(plo);
    if (!palo || !m_psw) return;
    for (const auto& up : m_psw->aploAll)
        if (FCidDerivedFrom(up->Cid(), 0x30) && FLoInParentChain(up.get(), palo) && PaabindOfAlo(static_cast<ALO*>(up.get())))
            SetAloBoneBindings(static_cast<ALO*>(up.get()), {});
}

// ---- GLB export: an object (with its children), its whole group, or a character with its skeleton

bool CEditor::FExportGlb(LO* plo, int kind, const std::string& path, std::string& error)
{
    if (!plo) { error = "nothing selected"; return false; }
    std::vector<ALO*> apalo;
    std::function<void(LO*)> add = [&](LO* p) {
        if (ALO* pa = dynamic_cast<ALO*>(p)) if (std::find(apalo.begin(), apalo.end(), pa) == apalo.end()) apalo.push_back(pa);
        if (const auto it = m_mpchildren.find(p); it != m_mpchildren.end())
            for (LO* c : it->second) add(c);
    };
    if (kind == 1)
    {
        if (const auto it = m_mpgroupRoots.find(plo->igrp); it != m_mpgroupRoots.end())
            for (LO* r : it->second) add(r);
    }
    else if (kind == 2) { if (ALO* pa = dynamic_cast<ALO*>(plo)) apalo.push_back(pa); }   // (its own model: the children are objects of their own)
    else add(plo);
    const glm::mat4 refInv = glm::inverse(MatWorld(plo));
    const float scale = m_importScale > 0 ? m_importScale : 100.0f;
    // game -> glTF: (x, y, z) -> (x, z, -y) / scale
    const glm::mat4 C = glm::scale(glm::mat4(glm::transpose(AxisToGame())), glm::vec3(1.0f / scale));
    const glm::mat4 Cinv = glm::inverse(C);
    // a character: the skeleton's root joint is its placement record; the bind pose is its records' worlds as they
    // stand (MatBindWorld), the animations are the game's poses relative to the same record (ExportAnimations)
    ALO* paloChar = kind == 2 ? dynamic_cast<ALO*>(plo) : nullptr;
    const XF* pxfPlace = paloChar && !paloChar->axf.empty() ? paloChar->axf[0].get() : nullptr;
    md::Scene sc;
    std::map<const SHD*, int> mpmat;
    std::map<const XF*, int> mpjoint;
    auto materialOf = [&](const SHD* pshd) -> int {
        if (const auto it = mpmat.find(pshd); it != mpmat.end()) return it->second;
        md::Material m;
        char sz[32];
        std::snprintf(sz, sizeof(sz), "shader_%X", pshd ? unsigned(pshd->id) : 0u);
        m.name = sz;
        const TEXSEL sel = TexselFromShd(pshd, 0, ItexColour(pshd));
        const TEXSEL sel0 = sel.pbmp ? sel : TexselFromShd(pshd, 0, 0);
        std::vector<uint8_t> rgba;
        std::string e2;
        if (sel0.pbmp && FDecodeTexture(sel0.pbmp, sel0.pclut, rgba, e2))
        {
            // (a texture-pack replacement: its full image instead)
            std::vector<uint8_t> abMod;
            int wMod = 0, hMod = 0;
            md::Image img = FLoadTextureMod(rgba, sel0.pbmp->dx, sel0.pbmp->dy, abMod, &wMod, &hMod) ? ImageFromDecoded(abMod, wMod, hMod, m.name)
                                                                                                     : ImageFromDecoded(rgba, sel0.pbmp->dx, sel0.pbmp->dy, m.name);
            for (std::size_t k = 3; k < img.rgba.size(); k += 4) if (img.rgba[k] < 250) { m.blend = true; break; }
            m.image = int(sc.images.size());
            sc.images.push_back(std::move(img));
        }
        const int i = int(sc.materials.size());
        sc.materials.push_back(m);
        mpmat[pshd] = i;
        return i;
    };
    auto jointOf = [&](const XF* pxf) -> int {
        if (const auto it = mpjoint.find(pxf); it != mpjoint.end()) return it->second;
        md::Joint j;
        const ALO* pa = pxf->palo;
        int ixf = 0;
        if (pa) for (std::size_t k = 0; k < pa->axf.size(); ++k) if (pa->axf[k].get() == pxf) ixf = int(k);
        j.name = std::string(pa ? SzClassRaw(pa->Cid()) : "XF") + "#" + std::to_string(pa ? pa->oid : 0) + "." + std::to_string(ixf);
        j.inverseBind = glm::inverse(C * refInv * MatBindWorld(pxf) * Cinv);
        const int i = int(sc.joints.size());
        sc.joints.push_back(j);
        mpjoint[pxf] = i;
        return i;
    };
    if (pxfPlace) jointOf(pxfPlace);   // (joint 0: the character's placement, the skeleton's root)
    std::size_t ctri = 0;
    for (ALO* palo : apalo)
    {
        if (!palo->pglobset) continue;
        const GLOBSET& gs = *palo->pglobset;
        for (std::size_t ig = 0; ig < gs.aglob.size(); ++ig)
        {
            const GLOB& glob = gs.aglob[ig];
            if (glob.meshk != MESHK_Strips) continue;
            const GLOB& geom = glob.iglobTemplate >= 0 && glob.avtx.empty() ? gs.aglob[size_t(glob.iglobTemplate)] : glob;
            const XF* pxfOwner = glob.oidOwner >= 0 ? PxfFindAlo(palo, glob.oidOwner) : nullptr;
            // (a character: a part that follows a record (else its placement) is bound to it as a one-bone skin)
            const XF* pxfRigid = kind == 2 && geom.abone.empty() ? (pxfOwner ? pxfOwner : palo->axf.empty() ? nullptr : palo->axf[0].get()) : nullptr;
            glm::mat4 m = pxfOwner ? pxfOwner->matWorld : pxfRigid ? pxfRigid->matWorld : MatWorldOfAlo(palo);
            if (glob.fMatrix) m = m * glob.dmat;
            // (a character's skinned part as the game binds it (bone.cpp BindAloBones): its vertices in its owner record's
            // space at the bind, else in the world's; no instance matrix (the bones' matrices replace it))
            if (kind == 2 && !geom.abone.empty()) m = pxfOwner ? pxfOwner->matWorld : glm::mat4(1.0f);
            const glm::mat4 M = C * refInv * m;
            const glm::mat3 N = glm::transpose(glm::inverse(glm::mat3(M)));
            // skinned: the bones' records as joints
            std::vector<int> ajointOfBone;
            const bool fRigid = pxfRigid != nullptr;
            const bool fSkin = !geom.abone.empty() || fRigid;
            if (fRigid) ajointOfBone.push_back(jointOf(pxfRigid));
            else if (fSkin)
                for (const GLOBBONE& b : geom.abone)
                {
                    LO* pl = PloFindSwNearest(palo, b.oid);
                    const XF* pxf = pl && FCidDerivedFrom(pl->Cid(), 0x30) ? PxfFindAlo(static_cast<ALO*>(pl), b.ibone) : nullptr;
                    ajointOfBone.push_back(pxf ? jointOf(pxf) : -1);
                }
            md::Mesh mesh;
            mesh.name = std::string(SzClassRaw(palo->Cid())) + "_" + std::to_string(palo->oid) + "_" + std::to_string(ig) + "@" + std::to_string(glob.n0) + "," +
                        std::to_string(int(glob.rp)) + "," + std::to_string(glob.fHidden ? 1 : 0);
            mesh.skinned = fSkin;
            for (const SUBGLOB& sub : geom.asubglob)
            {
                if (sub.indexCount == 0) continue;
                md::Primitive p;
                p.material = materialOf(sub.pshd);
                std::map<uint32_t, uint32_t> mpv;
                for (uint32_t k = 0; k < sub.indexCount; ++k)
                {
                    const uint32_t iv = geom.aindex[sub.firstIndex + k];
                    auto it = mpv.find(iv);
                    if (it == mpv.end())
                    {
                        const VTXGLOB& v = geom.avtx[iv];
                        it = mpv.emplace(iv, uint32_t(p.pos.size())).first;
                        p.pos.push_back(glm::vec3(M * glm::vec4(v.pos, 1.0f)));
                        glm::vec3 n = N * v.normal;
                        const float l = glm::length(n);
                        p.nrm.push_back(l > 0 ? n / l : glm::vec3(0, 1, 0));
                        p.uv.push_back(glm::vec2(v.uv.x, 1.0f - v.uv.y));
                        if (fSkin)
                        {
                            glm::u16vec4 j(0);
                            glm::vec4 w(0.0f);
                            float wsum = 0;
                            for (int q = 0; q < 4 && !fRigid; ++q)
                            {
                                // (a slot past the group's bones is its last one, as the renderer reads it: light.cpp)
                                const int ib = std::min(int(v.abone[q]), int(ajointOfBone.size()) - 1);
                                if (v.aw[q] == 0 || ib < 0 || ajointOfBone[size_t(ib)] < 0) continue;
                                j[q] = uint16_t(ajointOfBone[size_t(ib)]);
                                w[q] = v.aw[q] / 255.0f;
                                wsum += w[q];
                            }
                            if (wsum > 0) w /= wsum;
                            else if (!ajointOfBone.empty() && ajointOfBone[0] >= 0) { j[0] = uint16_t(ajointOfBone[0]); w[0] = 1.0f; }
                            p.joints.push_back(j);
                            p.weights.push_back(w);
                        }
                    }
                    p.idx.push_back(it->second);
                }
                ctri += p.idx.size() / 3;
                mesh.prims.push_back(std::move(p));
            }
            if (std::getenv("SLY2N_EDITOR_EXPORTDBG") && !mesh.prims.empty())
            {
                glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
                bool fSaa = false;
                for (const auto& p : mesh.prims) for (const auto& v : p.pos) { mn = glm::min(mn, v); mx = glm::max(mx, v); }
                for (const SUBGLOB& sub : geom.asubglob) fSaa |= sub.psaa != nullptr;
                std::printf("export %s owner %d matrix %d bones %zu rigid %d saa %d grf 0x%X box %.2f %.2f %.2f .. %.2f %.2f %.2f\n", mesh.name.c_str(), glob.oidOwner, int(glob.fMatrix),
                            geom.abone.size(), int(fRigid), int(fSaa), glob.grfGroup, mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
            }
            if (!mesh.prims.empty()) sc.meshes.push_back(std::move(mesh));
        }
    }
    if (sc.meshes.empty()) { error = "the selection has no drawn model to export"; return false; }
    // (raw clip channels: the limbs' targets as joints of their own, to rebuild the joints with IK constraints)
    if (paloChar && m_exportAnim == 1)
        for (const ALOPAIR& q : paloChar->apair)
            if (q.apxf[0] && q.apxf[1] && q.apxf[2]) jointOf(q.apxf[0]);
    // the joints' parents (the nearest joint up the records' references; a character's: else its root) and rest poses
    for (auto& [pxf, ij] : mpjoint)
    {
        const glm::mat4 world = C * refInv * MatBindWorld(pxf) * Cinv;
        int ip = -1;
        for (const XF* p = pxf->pxfRef; p; p = p->pxfRef)
            if (const auto it = mpjoint.find(p); it != mpjoint.end()) { ip = it->second; break; }
        if (ip < 0 && pxfPlace && pxf != pxfPlace) ip = mpjoint[pxfPlace];
        sc.joints[size_t(ij)].parent = ip;
        sc.joints[size_t(ij)].local = ip >= 0 ? sc.joints[size_t(ip)].inverseBind * world : world;
    }
    // (parents before children, as the writer expects)
    {
        std::vector<int> depth(sc.joints.size(), 0);
        for (std::size_t i = 0; i < sc.joints.size(); ++i)
            for (int p = sc.joints[i].parent; p >= 0 && depth[i] < 1000; p = sc.joints[size_t(p)].parent) ++depth[i];
        std::vector<int> order(sc.joints.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return depth[size_t(a)] < depth[size_t(b)]; });
        std::vector<int> newOf(order.size());
        for (std::size_t i = 0; i < order.size(); ++i) newOf[size_t(order[i])] = int(i);
        std::vector<md::Joint> aj(order.size());
        for (std::size_t i = 0; i < order.size(); ++i) { aj[i] = sc.joints[size_t(order[i])]; if (aj[i].parent >= 0) aj[i].parent = newOf[size_t(aj[i].parent)]; }
        sc.joints = std::move(aj);
        for (md::Mesh& mesh : sc.meshes)
            for (md::Primitive& p : mesh.prims)
                for (glm::u16vec4& j : p.joints)
                    for (int q = 0; q < 4; ++q) j[q] = uint16_t(newOf[j[q]]);
        for (auto& [pxf, ij] : mpjoint) ij = newOf[size_t(ij)];
    }
    m_exportAnimInfo.clear();
    if (paloChar)
    {
        EXPORTRIG rig;
        BuildRig(paloChar, sc, mpjoint, C, refInv, m_exportAnim == 1, rig.apxf, rig.amatFrame);
        rig.C = C;
        rig.fSolve = m_exportAnim == 0;
        ExportAnimations(plo, sc, rig);
    }
    if (!md::saveGlb(ce::pathOf(path), sc, error)) return false;
    m_lastExport = path;
    Status("exported " + std::to_string(sc.meshes.size()) + " meshes, " + std::to_string(ctri) + " triangles, " + std::to_string(sc.materials.size()) + " materials, " +
           std::to_string(sc.joints.size()) + " joints, " + std::to_string(sc.anims.size()) + " animations to " + path +
           (paloChar ? " (animations: " + m_exportAnimInfo + ")" : std::string()));
    return true;
}

// ---- the UI: the inspector's Model section, the Models tab, file dialogs, getting started, shortcuts

void CEditor::OpenFileDialog(const char* title, const char* filters, const std::string& start, std::function<void(const std::string&)> fn)
{
    m_fnFile = std::move(fn);
    ImGuiFileDialog::Instance()->OpenDialog("edfile", title, filters, start.empty() ? std::string(".") : start, 1, nullptr, ImGuiFileDialogFlags_Modal);
}

void CEditor::DrawFileDialogs()
{
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    if (ImGuiFileDialog::Instance()->Display("edfile", ImGuiWindowFlags_NoCollapse, ImVec2(size.x * 0.5f, size.y * 0.5f), ImVec2(size.x * 0.9f, size.y * 0.9f)))
    {
        if (ImGuiFileDialog::Instance()->IsOk() && m_fnFile)
        {
            const std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
            auto fn = std::move(m_fnFile);
            m_fnFile = nullptr;
            ImGuiFileDialog::Instance()->Close();
            fn(path);
            return;
        }
        m_fnFile = nullptr;
        ImGuiFileDialog::Instance()->Close();
    }
}

void CEditor::ForgetThumbnails()
{
    for (auto& [k, tex] : m_mpthumb) if (tex) glDeleteTextures(1, &tex);
    m_mpthumb.clear();
}

unsigned CEditor::ThumbOf(const SHD* pshd, int* pdx, int* pdy)
{
    const TEXSEL sel = TexselFromShd(pshd, 0, ItexColour(pshd));
    if (!sel.pbmp) return 0;
    *pdx = sel.pbmp->dx;
    *pdy = sel.pbmp->dy;
    const auto key = std::make_pair(static_cast<const void*>(sel.pbmp), static_cast<const void*>(sel.pclut));
    if (const auto it = m_mpthumb.find(key); it != m_mpthumb.end()) return it->second;
    std::vector<uint8_t> rgba;
    std::string e;
    unsigned tex = 0;
    if (FDecodeTexture(sel.pbmp, sel.pclut, rgba, e))
    {
        const md::Image img = ImageFromDecoded(rgba, sel.pbmp->dx, sel.pbmp->dy, "");
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    m_mpthumb[key] = tex;
    return tex;
}

namespace
{
void Tip(const char* sz)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", sz);
}
const char* SzExportKind(int k) { return k == 0 ? "the object (with its children)" : k == 1 ? "its whole group (the world part)" : "a character (skeleton + animations)"; }

// A character export's animation options (ExportAnimations): as the game shows them, or the clips' channels only
void DrawCharAnimOptions(int& mode, int& fps, bool& fReduce)
{
    ImGui::SetNextItemWidth(std::min(260.0f, ImGui::GetContentRegionAvail().x));
    const char* asz[2] = {"Animations as in the game", "Clip channels only (no IK)"};
    if (ImGui::BeginCombo("##exportanim", asz[mode == 1 ? 1 : 0]))
    {
        for (int k = 0; k < 2; ++k) if (ImGui::Selectable(asz[k], mode == k)) mode = k;
        ImGui::EndCombo();
    }
    Tip("As in the game: every clip played through the game's own update (the limbs' two-bone joints solved, links and "
        "chains, squash and stretch) and each joint baked per frame: Blender shows what the game shows.\n"
        "Clip channels only: the clips' keys without the solvers (the limbs stay at rest while the hands / feet move); "
        "the limbs' targets are extra joints and the root's custom properties (safehouse_ik) list each limb's joints, to "
        "rebuild them with IK constraints.");
    bool f60 = fps >= 60;
    if (ImGui::Checkbox("60 keys/s", &f60)) fps = f60 ? 60 : 30;
    Tip("60: a key at every game step (exact at each frame the game draws); 30: half as many before the reduction.");
    ImGui::SameLine();
    ImGui::Checkbox("Fewer keys", &fReduce);
    Tip("Keys dropped where the joints stay within 0.5 mm (at 1 unit = 1 m) of the game's pose.");
}
}  // namespace

void CEditor::DrawModelSection(LO* plo)
{
    ALO* palo = dynamic_cast<ALO*>(plo);
    const auto it = m_mpobj.find(plo);
    if (!palo || it == m_mpobj.end() || it->second.iglobset < 0) return;
    if (!ImGui::CollapsingHeader("Model and materials", ImGuiTreeNodeFlags_DefaultOpen)) return;
    std::size_t ctri = 0, cglob = 0;
    if (palo->pglobset)
        for (const GLOB& g : palo->pglobset->aglob)
        {
            ++cglob;
            ctri += (g.iglobTemplate >= 0 && g.aindex.empty() ? palo->pglobset->aglob[size_t(g.iglobTemplate)].aindex.size() : g.aindex.size()) / 3;
        }
    ImGui::TextColored(st::vec(st::muted), "%zu parts, %zu triangles%s", cglob, ctri, plo->oidInstance > 0 && plo->oidInstance != plo->oid ? "" : "");
    if (ImGui::Button("Replace model..."))
        OpenFileDialog("Replace the model", "Models{.glb,.gltf,.fbx}", m_importPath, [this](const std::string& path) {
            std::string e;
            std::snprintf(m_importPath, sizeof(m_importPath), "%s", path.c_str());
            if (!FImportModel(path, m_plSel, false, e)) Status("import: " + e);
        });
    Tip("A .glb / .gltf / .fbx file in place of this model (options in World > Models). A shared group's model changes in every instance.");
    ImGui::SameLine();
    if (ImGui::Button("Export GLB..."))
        OpenFileDialog("Export as GLB", ".glb", m_exportPath, [this](const std::string& path) {
            std::string e;
            std::snprintf(m_exportPath, sizeof(m_exportPath), "%s", path.c_str());
            if (!FExportGlb(m_plSel, m_exportKind, path, e)) Status("export: " + e);
        });
    Tip("Writes the model, its textures (the texture pack's where there is one) and, for a character, the skeleton and animations.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##exportkind", m_exportKind == 0 ? "object" : m_exportKind == 1 ? "group" : "character"))
    {
        for (int k = 0; k < 3; ++k) if (ImGui::Selectable(SzExportKind(k), m_exportKind == k)) m_exportKind = k;
        ImGui::EndCombo();
    }
    if (m_exportKind == 2) DrawCharAnimOptions(m_exportAnim, m_exportFps, m_exportReduce);
    const std::vector<SHD*> apshd = ApshdOfAlo(palo);
    for (std::size_t i = 0; i < apshd.size(); ++i)
    {
        SHD* pshd = apshd[i];
        ImGui::PushID(int(i));
        int dx = 0, dy = 0;
        const unsigned tex = ThumbOf(pshd, &dx, &dy);
        if (tex) ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(tex)), ImVec2(44, 44));
        else ImGui::Dummy(ImVec2(44, 44));
        if (tex && ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(tex)), ImVec2(256, 256.0f * float(dy) / float(std::max(1, dx))));
            ImGui::Text("%dx%d", dx, dy);
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("Material %zu", i);
        ImGui::SameLine();
        ImGui::TextColored(st::vec(st::muted), "%s, %dx%d", pshd->shdk == 0 ? "unlit" : pshd->shdk == 1 ? "lit" : "special", dx, dy);
        if (ImGui::SmallButton("Texture..."))
            OpenFileDialog("Replace the texture", "Images{.png,.jpg,.jpeg}", m_texPath, [this, i](const std::string& path) {
                std::string e;
                std::snprintf(m_texPath, sizeof(m_texPath), "%s", path.c_str());
                if (!FImportTexture(path, m_plSel, int(i), m_texMode, e)) Status("texture: " + e);
            });
        Tip("A PNG / JPEG for this material. Texture pack (default): at its own size, every model using the texture. Game data: resized into the bitmap.");
        ImGui::SameLine();
        if (ImGui::SmallButton("PNG..."))
            OpenFileDialog("Save the texture as PNG", ".png", m_texPath, [this, pshd](const std::string& path) {
                const TEXSEL sel = TexselFromShd(pshd, 0, ItexColour(pshd));
                std::vector<uint8_t> rgba;
                std::string e;
                if (!sel.pbmp || !FDecodeTexture(sel.pbmp, sel.pclut, rgba, e)) { Status("png: no texture"); return; }
                if (md::savePng(ce::pathOf(path), ImageFromDecoded(rgba, sel.pbmp->dx, sel.pbmp->dy, ""), e)) Status("wrote " + path);
                else Status("png: " + e);
            });
        Tip("The texture as the game decodes it (paint over it and load it back with Texture...).");
        ImGui::SameLine();
        bool fLit = pshd->shdk == 1;
        if ((pshd->shdk == 0 || pshd->shdk == 1) && ImGui::Checkbox("lit", &fLit))
        {
            std::string e;
            const uint32_t argba[3] = {pshd->argba[0], pshd->argba[1], pshd->argba[2]};
            if (!FSetMaterial(m_plSel, int(i), fLit ? 1 : 0, pshd->grfshd, argba, pshd->atex.empty() ? 0 : pshd->atex[0].grftex, e)) Status("material: " + e);
        }
        Tip("Lit by the level's lights (as characters and props) or flat (the texture times the vertex colours).");
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(160);
    ImGui::Combo("texture import", &m_texMode, "texture pack (full size)\0game data (resized)\0");
    Tip("Texture pack: Textures/<name>.png in the package (the game's own texture untouched, any size, mipmapped). Game data: the PNG re-encoded into the bitmap and its palettes.");
}

void CEditor::DrawModels()
{
    ImGui::TextWrapped("Bring your own models and textures in, or take the game's out (glTF 2.0 .glb / .gltf, FBX). Blender: export glTF with +Y up (the default); 1 unit = 1 metre.");
    ImGui::SeparatorText("Import options");
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat("scale (game units per file unit)", &m_importScale, 1.0f, 1.0f, 10000.0f, "%.0f");
    Tip("The game measures in centimetres: 100 makes a 1 m model 100 units (Sly is about 110 tall).");
    ImGui::SetNextItemWidth(120);
    ImGui::SliderInt("largest texture in the game data", &m_importMaxTex, 16, 512);
    Tip("The game's copy of each texture (indexed, 256 colours). The full image also goes into the package's Textures/, which the game draws instead.");
    ImGui::Checkbox("lit (like the game's props)", &m_importLit);
    ImGui::SameLine();
    ImGui::Checkbox("cel outline", &m_importOutline);
    Tip("The black cel border along the silhouette, made from the mesh's shared edges.");
    ImGui::SetNextItemWidth(160);
    ImGui::Combo("collision", &m_importCollision, "kept as it is\0none\0a box around the model\0");
    Tip("What Sly stands on: the replaced object's own collision, none, or a box fitted to the new model.");
    ImGui::SeparatorText("Import");
    if (ImGui::Button("As a new object..."))
        OpenFileDialog("Import a model as a new object", "Models{.glb,.gltf,.fbx}", m_importPath, [this](const std::string& path) {
            std::string e;
            std::snprintf(m_importPath, sizeof(m_importPath), "%s", path.c_str());
            if (!FImportModel(path, m_plSel, true, e)) Status("import: " + e);
        });
    Tip("Placed in front of the camera (on the ground below), in the level's group: a copy of the selected plain object, or of a plain one of the level.");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_plSel == nullptr);
    if (ImGui::Button("In place of the selection's model..."))
        OpenFileDialog("Replace the model", "Models{.glb,.gltf,.fbx}", m_importPath, [this](const std::string& path) {
            std::string e;
            std::snprintf(m_importPath, sizeof(m_importPath), "%s", path.c_str());
            if (!FImportModel(path, m_plSel, false, e)) Status("import: " + e);
        });
    ImGui::EndDisabled();
    ImGui::SeparatorText("Export");
    ImGui::BeginDisabled(m_plSel == nullptr);
    for (int k = 0; k < 3; ++k)
    {
        if (ImGui::RadioButton(SzExportKind(k), m_exportKind == k)) m_exportKind = k;
    }
    if (m_exportKind == 2) DrawCharAnimOptions(m_exportAnim, m_exportFps, m_exportReduce);
    if (ImGui::Button("Export the selection as GLB..."))
        OpenFileDialog("Export as GLB", ".glb", m_exportPath, [this](const std::string& path) {
            std::string e;
            std::snprintf(m_exportPath, sizeof(m_exportPath), "%s", path.c_str());
            if (!FExportGlb(m_plSel, m_exportKind, path, e)) Status("export: " + e);
        });
    ImGui::EndDisabled();
    if (!m_lastExport.empty()) ImGui::TextColored(st::vec(st::muted), "Last: %s", m_lastExport.c_str());
    ImGui::SeparatorText("Round trip");
    ImGui::TextWrapped("A character exported here keeps its joints' names (\"MURRAY#9.31\": object 9, record 31; a limb's joints add "
                       "their part, \"_thigh_L\"; \"_deform\" joints carry a joint's squash and stretch). Edit the mesh in Blender, keep "
                       "the armature and the names, and import it back in place of the character's model: it stays skinned and animated. "
                       "Imported parts have no shader animations (blinks, scrolling) and use the level's lights.");
}

void CEditor::DrawHelp()
{
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    if (m_fShowHelp)
    {
        ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f - (m_fShowKeys ? 200.0f : 0.0f), size.y * 0.42f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(1.0f);
        if (ImGui::Begin("Getting started", &m_fShowHelp, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
        {
            auto step = [](const char* head, const char* body) {
                ImGui::TextColored(st::vec(st::accent), "%s", head);
                ImGui::Indent();
                ImGui::TextWrapped("%s", body);
                ImGui::Unindent();
                ImGui::Spacing();
            };
            step("1  Look around", "Hold the right mouse button and use W A S D (Q / E down / up, Shift faster, the wheel sets the speed). Click an object to select it; F frames it.");
            step("2  Change things", "Drag the gizmo's arrows (W move, E rotate, R scale) or type numbers in the Inspector. Options, curves, scripts, text and missions have their own panels.");
            step("3  Add things", "World > Create places copies of the level's objects; World > Models imports your own .glb / .fbx models; the Inspector's Model section replaces a model or a texture.");
            step("4  Try it", "Play test starts the game in this world with your project. Ctrl+Z undoes anything, also after reopening.");
            step("5  Share it", "Ctrl+S saves the project: it is a mod package. Project > Install puts it in Sly 2's mods folder; Export as .zip makes a file to share.");
            ImGui::Separator();
            ImGui::Checkbox("Show at start", &m_fHelpAtStart);
            ImGui::SameLine(ImGui::GetWindowWidth() - 150);
            if (ImGui::Button("Keyboard shortcuts")) m_fShowKeys = true;
        }
        ImGui::End();
    }
    if (m_fShowKeys)
    {
        ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f + (m_fShowHelp ? 330.0f : 0.0f), size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(1.0f);
        if (ImGui::Begin("Keyboard shortcuts", &m_fShowKeys, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
        {
            static const char* const s_akey[][2] = {
                {"Right mouse + W A S D", "fly"}, {"Q / E (while flying)", "down / up"}, {"Shift (while flying)", "faster"}, {"Mouse wheel", "flying speed"},
                {"Left click", "select (on a curve: a point)"}, {"W / E / R", "move / rotate / scale"}, {"F", "frame the selection"},
                {"Ctrl+D", "duplicate"}, {"Delete", "delete"}, {"Ctrl+Z / Ctrl+Y", "undo / redo (Ctrl+Shift+Z redo)"}, {"Ctrl+S", "save"},
                {"F1", "getting started"}, {"Enter (in a field)", "apply the value"},
            };
            if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
            {
                for (const auto& k : s_akey)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(st::vec(st::accent), "%s", k[0]);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(k[1]);
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
}

}  // namespace edimpl

#endif
