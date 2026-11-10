// See emitter.h. Ported from Sly2-Reference game/emitter_p3.cpp (0x159118..0x15C690), emitter.cpp (0x15CAD8),
// m_137FB8_p3.cpp (0x138FF8..0x139DC0), m_13A1E8_p3.cpp (0x13A1E8 / 0x13A688), m_138F08.cpp (the GIF packets) and
// runtime/native_blip_geometry.cpp (the VU1 programs' expansion), and from the original's code where the reference
// has no port (the option getters / setters 0x258658..0x258EC8, 0x2584A8, 0x2586D8, disassembled), in plain
// IEEE float.

#include "fastpaths.h"
#include "emitter.h"
#include "worldglobals.h"
#include "texmod.h"
#include "animfind.h"
#include "jt.h"
#include "cm.h"

#include "activate.h"
#include "clock.h"
#include "disc.h"
#include "display.h"
#include "gl.h"
#include "lo.h"
#include "render.h"
#include "scene.h"
#include "shd.h"
#include "sound.h"
#include "sw.h"
#include "update.h"
#include "vecmat.h"
#include "worldaccess.h"
#include "xfmath.h"
#include "crv.h"
#include "bsp.h"
#include "mat.h"
#include "so.h"
#include "xfphys.h"

glm::vec3 VelWaterAtPoint(ALO* pw, const glm::vec3& pos);  // water.cpp (0x228C18)

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

PARTICLESTATS g_particlestats;

// (PC addition, debug menu only, pcloader.h: a guest's copies of a kept world's emitters use that world's shaders)
constinit const SHD* (*g_pfnPcShdOfLo)(const LO* plo, int32_t id) = nullptr;
constinit LO* (*g_pfnPcFindOid)(const LO* ploNear, int32_t oid) = nullptr;

namespace
{
constexpr float PI = 3.14159265f;
constexpr float FLT_BIG = 3.4028235e38f;
constexpr uint32_t VA_AEOPID = 0x2AC880;

// ------------------------------------------------------------ helpers of util.c / vec.c
// (NRandom / GRandInRange: xfmath.h, the game's shared C library rand)
// orig: 0x0011D3F8 NRandInRange
int32_t NRandInRange(int32_t nLow, int32_t nHigh)
{
    const int32_t d = nHigh - (nLow - 1);
    return d == 0 ? nLow : nLow + NRandom() % d;
}
float GModPositive(float g, float d)
{
    float m = std::fmod(g, d);
    if (m < 0) m += d;
    return m;
}
// orig: 0x0011CF60 RadNormalize
float RadNormalize(float rad)
{
    if (rad < -PI || PI < rad) rad = GModPositive(rad + PI, 2.0f * PI) - PI;
    return rad;
}
// orig: 0x0011E208 SetVectorSphere
glm::vec3 SetVectorSphere(float radPan, float radTilt, float s)
{
    return glm::vec3(std::cos(radTilt) * std::cos(radPan) * s, std::cos(radTilt) * std::sin(radPan) * s, std::sin(radTilt) * s);
}
glm::vec3 NormalizeOr(const glm::vec3& v, const glm::vec3& vZero)
{
    const float l = glm::length(v);
    return l < 0.0001f ? vZero : v / l;
}
// orig: 0x0011E2D8 BuildOrthonormalVectors (across = up x dir (upAlt when along dir), dir x across)
void BuildOrthonormalVectors(const glm::vec3& dir, const glm::vec3& up, const glm::vec3& upAlt, glm::vec3& a0, glm::vec3& a1)
{
    glm::vec3 across = glm::cross(up, dir);
    if (glm::dot(across, across) < 1.0e-4f) across = glm::cross(upAlt, dir);  // (the original's bound on the square)
    a0 = NormalizeOr(across, glm::vec3(0.0f));
    a1 = NormalizeOr(glm::cross(dir, a0), glm::vec3(0.0f));
}
// orig: 0x0011D5B0 PrescaleClq (src, dst, ru, du): dst(x) = src(du + ru x)
void PrescaleClq(const float src[3], float dst[3], float ru, float du)
{
    dst[0] = src[0] + du * (src[1] + src[2] * du);
    dst[1] = ru * (src[1] + 2.0f * src[2] * du);
    dst[2] = ru * ru * src[2];
}

// ------------------------------------------------------------ byte images
struct IMG
{
    std::vector<uint8_t> ab;
    explicit IMG(size_t cb = 0) : ab(cb, 0) {}
    template <class T> T Rd(uint32_t off) const
    {
        T v{};
        if (off + sizeof(T) <= ab.size()) std::memcpy(&v, ab.data() + off, sizeof(T));
        return v;
    }
    template <class T> void Wr(uint32_t off, T v)
    {
        if (off + sizeof(T) <= ab.size()) std::memcpy(ab.data() + off, &v, sizeof(T));
    }
    float F(uint32_t off) const { return Rd<float>(off); }
    int32_t I(uint32_t off) const { return Rd<int32_t>(off); }
    uint32_t U(uint32_t off) const { return Rd<uint32_t>(off); }
    glm::vec3 V(uint32_t off) const { return glm::vec3(F(off), F(off + 4), F(off + 8)); }
    void WrV(uint32_t off, const glm::vec3& v) { Wr(off, v.x); Wr(off + 4, v.y); Wr(off + 8, v.z); }
    void CopyFromElf(uint32_t off, uint32_t va, uint32_t cb)
    {
        for (uint32_t k = 0; k < cb; k += 4) Wr(off + k, g_disc.U32Elf(va + k));
    }
};

// The objects an EMITB refers to (0x2586D8's 0x14-byte record at EMITB +8): the source object, the surface (kind-2
// positions) and the three of the particle kind.
struct EMITS
{
    LO* aplo[5] = {};
};

struct PSYS;
struct EMITTERN
{
    LO* plo = nullptr;              // the world's emitter (null: a group made by PostEmitterLoad)
    const LO* ploMember = nullptr;  // (PC: a group's first member, for g_pfnPcShdOfLo)
    IMG e{0x160};                   // EMITTER +0x00..+0x160 (the fields from +0xE0 on)
    IMG b{0x100};                   // its EMITB (+0xE4)
    EMITS s;                        // EMITB +8
    std::array<glm::vec4, 2> abox{};  // EMITB +0x30 for position kind 1 (lo, hi)
    bool fBox = false;
    const LOXEMIT::EMITMESH* pmesh = nullptr;  // EMITB +0x30 for position kind 3
    std::vector<uint32_t> argbaColor;           // EMITB +0xDC {n, rgba[n]} (none: the shader's colour)
    PSYS* ppsys = nullptr;          // +0x150 (an explosion's +0x104)
    struct RIPG* pripg = nullptr;   // +0x14C (an explosion's +0x100)
    bool fExpl = false;             // an explosion (class 0x20 family): bursts only (0x15C690)
    // (QoL draw distance above 1x: its runs beyond the original's range, UpdateEmitter) its own random stream and
    // its own run-time fields (UpdateEmitter)
    bool fPrivateInit = false;
    PSYS* ppsysPrivate = nullptr;   // (and its own particle system: the original's keeps its count)
    uint32_t uRandPrivate = 0;
    int32_t anRunPrivate[5] = {};
    IMG snd{0};                     // an explosion's +0xF0 sound record (0x15C650: sfx, falloff start / end, volume,
                                    // pitch; empty: none)
    glm::mat4 matPrev{0.0f};        // the source's last world transform (its velocity for 0x22D348)
    glm::mat4 matPrevPrivate{0.0f}; // (drawdist-2: matPrev of the runs the update table leaves out, which keep matPrev)
    const glm::mat4* pmatFrame = nullptr;   // EMITB +0x1C: a burst's frame (0x15B7E8: pos = R pos + t, vel = R vel)
    EMITTERN* pgroup = nullptr;     // the group (+0x118's object) holding it in its +0x11C list
    std::vector<EMITTERN*> amember; // +0x11C
    glm::mat4 matGroup{1.0f};       // a group's own transform (slot 30 with the first member's rotation)
    bool fInit = false;
};

// A particle block (0x1090 bytes: 43 particles, one buffer of the original's two).
struct PBLOCK
{
    int c = 0;
    glm::vec4 apos[43];             // xyz, w: tCreated
    glm::vec4 avel[43];             // xyz, w: tDestroy
    glm::vec4 aextra[43];           // colour index (bits), texture index (bits), spin rate, roll angle
    glm::vec4 aspline[43][2];       // +0x1088: update kind 2's end point and end velocity (0x15B9F0)
};

// The particle system (class 0x1C, 0x460 bytes).
struct PSYS
{
    EMITTERN* pemitterPar = nullptr;   // +0xE0 (slot 51 0x139900: the emitter (group) it draws for)
    std::vector<std::unique_ptr<PBLOCK>> ablk;   // +0x448
    int cparticle = 0;              // +0x444
    // 0x1394C8
    int kDraw = 0;                  // +0x43C: 0 billboard, 1 flying, 2 rolling
    float gFlying = 1.0f;           // +0x440
    std::vector<std::array<uint8_t, 4>> argba;   // +0x3B0 / +0x3B4: reversed, halved
    bool fColorPairs = false;       // +0x434
    const SHD* pshd = nullptr;      // the texture's draw record
    bool fAdditive = false;         // +0x438: 0x2DD600 (ALPHA (Cs - 0) x As + Cd) else 0x2DD5C0 ((Cs - Cd) x As + Cd)
    std::vector<int> aitex;         // +0x1A0 / +0x1B0: the table (a power of 2 <= 16) -> the shader's frame
    float clqTex[3] = {}, clqScale[3] = {}, clqAlpha[3] = {}, clqColor[3] = {};   // +0x140 / +0x110 / +0x130 / +0x150
    float sSize = 0;                // +0x120
    int kUpdate = 0;                // +0x160
    glm::vec3 accel{0.0f}, axisSpin{0, 0, 1};    // +0x170 (0x15BF40)
    float gSpin = 0, gDrag = 0;
    glm::vec3 posCenter{0.0f};      // +0xF0
    float sRadius = 0;              // +0x100
    bool fDead = false;
};

std::vector<std::unique_ptr<EMITTERN>> s_aemitter;
WORLD_TABLE(s_aemitter);   // (PC: moved aside while a kept world loads, worldglobals.h)
std::map<const LO*, EMITTERN*> s_mpemitter;
WORLD_TABLE(s_mpemitter);   // (PC: moved aside while a kept world loads, worldglobals.h)
std::vector<std::unique_ptr<PSYS>> s_apsys;
WORLD_TABLE(s_apsys);   // (PC: moved aside while a kept world loads, worldglobals.h)
const SW* s_psw = nullptr;
WORLD_VALUE(s_psw);   // (PC: moved aside while a kept world loads, worldglobals.h)
uint32_t s_serialSw = 0;   // the world s_psw was (a new world may reuse its address)
WORLD_VALUE(s_serialSw);   // (PC: moved aside while a kept world loads, worldglobals.h)
// Native scene lookups are acceleration only. Original 0x1CF338 -> 0x1A5EE8 draws the live model;
// 0x1CDE80 removes RIP groups with their world. A recreated scene must not reuse old placements.
uint64_t s_sceneLookupGeneration = 0;
bool FOtherSw(const SW* psw) { return s_psw != psw || !psw || psw->serial != s_serialSw; }
glm::vec3 s_posCamera{0.0f};     // the camera of the last frame drawn (CM +0x90)
bool s_fGameCamera = false;      // the game loop runs the world pass: its emitters test against g_cm
// the camera's rows (CM +0x60: view, left, up): the game's camera in the game loop, else the viewer's
glm::mat3 MatParticleCamera();
glm::mat3 s_matCamera{1.0f};     // its rows (CM +0x60): view, left, up
glm::mat3 MatParticleCamera() { return s_fGameCamera ? g_cm.mat : s_matCamera; }
bool s_fAllActive = false;       // the viewer: every emitter counts as active (no world update table)
bool FActive(const LO* plo) { return plo && (s_fAllActive || FLoActive(plo)); }

// orig: 0x001EC8D0 (the draw record (0x1EB678, the shaders flagged 0x40) of an id, else the first one): natively
// the shader itself, whose draw record is {id, +6, +5 flags (1 additive), +8 colour, the frames' TEX0}.
const SHD* PshdOfId(int32_t id)
{
    const SHD* pFirst = nullptr;
    for (const auto& p : g_shds.ashd)
    {
        if (!(p->grfshd & 0x40)) continue;
        if (!pFirst) pFirst = p.get();
        if (p->id == id) return p.get();
    }
    return pFirst;
}

// The emitter's world transform (vt slot 31 / 32: LOX +0x50 record x +0x60).
glm::mat4 MatWorldOfLo(const LO* plo)
{
    if (!plo) return glm::mat4(1.0f);
    if (const LOX* px = dynamic_cast<const LOX*>(plo))
    {
        // the record it refers to, else its parent's placement (TODO(world): the LOX's own rule 0x231768)
        if (px->pxf) return px->pxf->matWorld * px->mat;
        if (plo->paloParent) return MatWorldOfAlo(plo->paloParent) * px->mat;
        return px->mat;
    }
    if (FCidDerivedFrom(plo->Cid(), 0x30)) return MatJtLo(const_cast<LO*>(plo));  // vt slot 31 (the object record +0x60)
    return glm::mat4(1.0f);
}
glm::mat4 MatWorldOfEmitter(const EMITTERN* pe) { return pe->plo ? MatWorldOfLo(pe->plo) : pe->matGroup; }

LO* PloFindOid(int32_t oid, const LO* ploNear)
{
    if (oid == -1 || !s_psw) return nullptr;
    if (g_pfnPcFindOid && ploNear)   // (PC addition, debug menu only: a guest's copy finds its own world's objects)
        if (LO* plo = g_pfnPcFindOid(ploNear, oid)) return plo;
    // orig 0x15E938 (kind 4: the nearest by the object's namespace, with 0x15E3E8's channel test): e.g. the blowing
    // paper's model 21059 is its group's clone (channel 0x117 as the emitter), not the template (0x11). Natively the
    // channel is the load group; the full test (k, the namespace lists) is not modelled, so: the emitter's channel
    // first, else any
    if (LO* plo = PloFindSwNearest(const_cast<LO*>(ploNear), oid, true)) return plo;
    return PloFindSwNearest(const_cast<LO*>(ploNear), oid);
}

// ------------------------------------------------------------ the option getters and setters
// orig: 0x00258658 (the EMITB for a change: fValuesChanged)
IMG* PimgEmitb(EMITTERN* pe, bool fModify)
{
    if (fModify) pe->e.Wr<int32_t>(0xE0, 1);
    return &pe->b;
}
// orig: 0x00258898 (the kind-0 block EMITB +0xC0: made from 0x2BDE10 when the kind is still -1)
uint32_t OffKind0(EMITTERN* pe, bool fModify)
{
    PimgEmitb(pe, true);
    if (fModify && pe->b.I(4) == -1)
    {
        pe->b.Wr<int32_t>(4, 0);
        pe->b.CopyFromElf(0xC0, 0x2BDE10, 0x40);
    }
    return pe->b.I(4) == 0 ? 0xC0 : 0xFFFFFFFF;
}
// orig: 0x00258920 (the kind-1 block, from 0x2BDE70)
uint32_t OffKind1(EMITTERN* pe, bool fModify)
{
    PimgEmitb(pe, true);
    if (fModify && pe->b.I(4) == -1)
    {
        pe->b.Wr<int32_t>(4, 1);
        pe->b.CopyFromElf(0xC0, 0x2BDE70, 0x40);
    }
    return pe->b.I(4) == 1 ? 0xC0 : 0xFFFFFFFF;
}
// orig: 0x00258D98 (the kind-0 block's payload tag: 1 a float (0x2BDE00), 2 8 bytes (0x2BDDF0))
void SetKind0Payload(EMITTERN* pe, int n)
{
    const uint32_t off = OffKind0(pe, true);
    if (off == 0xFFFFFFFF || pe->b.I(off) == n || pe->b.I(0xC0) != 0) return;
    pe->b.Wr<int32_t>(off, n);
    if (n == 2) pe->b.CopyFromElf(off + 0x30, 0x2BDDF0, 8);
    else if (n == 1) pe->b.CopyFromElf(off + 0x30, 0x2BDE00, 4);
}
// orig: 0x00258B70 (the kind-1 block's payload tag: 2 (0x2BDE50, 8), 3 (0x2BDE58, 4), 8 (0x2BDE60, 12))
void SetKind1Payload(EMITTERN* pe, int n)
{
    const uint32_t off = OffKind1(pe, true);
    if (off == 0xFFFFFFFF || pe->b.I(off) == n || pe->b.I(off) != -1) return;
    pe->b.Wr<int32_t>(off, n);
    if (n == 2) pe->b.CopyFromElf(off + 0x30, 0x2BDE50, 8);
    else if (n == 3) pe->b.CopyFromElf(off + 0x30, 0x2BDE58, 4);
    else if (n == 8) pe->b.CopyFromElf(off + 0x30, 0x2BDE60, 12);
}
// orig: 0x00258A00 (the position kind, once from 0: 1 a box, 3 a mesh (loaded by 0x258598))
void SetPositionKind(EMITTERN* pe, int n)
{
    const int k = pe->b.I(0x10);
    if (k == n || k != 0) return;
    PimgEmitb(pe, true)->Wr<int32_t>(0x10, n);
    if (n == 1) pe->fBox = true;
}

// Where an option's value goes: an image and an offset (null: nowhere natively).
struct FIELDREF
{
    IMG* pimg = nullptr;
    uint32_t off = 0;
    bool fBox = false;   // into the box (getter 0x258B00)
};
// The explosions' (class 0x20 family, loader 0x259030) getters and setters mirror the emitters' one for one.
uint32_t PfnEmitterOfExpl(uint32_t pfn)
{
    switch (pfn)
    {
    case 0x2590F0: return 0x258658;
    case 0x259330: return 0x258898;
    case 0x2593B8: return 0x258920;
    case 0x259560: return 0x258AC8;
    case 0x259598: return 0x258B00;
    case 0x2595D0: return 0x258B38;
    case 0x2598F8: return 0x258E60;
    case 0x259960: return 0x258EC8;
    case 0x2596F8: return 0x258C60;
    case 0x259760: return 0x258CC8;
    case 0x2597C8: return 0x258D30;
    case 0x259440: return 0x2589A8;
    case 0x259498: return 0x258A00;
    case 0x259608: return 0x258B70;
    case 0x259830: return 0x258D98;
    default: return pfn;
    }
}

FIELDREF FieldOfGetter(EMITTERN* pe, uint32_t pfnGet, uint32_t c18)
{
    pfnGet = PfnEmitterOfExpl(pfnGet);
    FIELDREF r;
    uint32_t base = 0;
    switch (pfnGet)
    {
    case 0: r.pimg = &pe->e; base = 0; break;
    case 0x159A08: r.pimg = &pe->e; break;                      // PemitterEnsureEmitter
    case 0x258658: r.pimg = PimgEmitb(pe, true); break;
    case 0x258898: base = OffKind0(pe, true); r.pimg = &pe->b; break;
    case 0x258920: base = OffKind1(pe, true); r.pimg = &pe->b; break;
    case 0x15C650:   // the explosion's sound record +0xF0 (made on first use: 0x1FAF70)
        if (pe->snd.ab.empty())
        {
            pe->snd = IMG(0x14);
            pe->snd.Wr<int32_t>(0, -1);
            pe->snd.Wr<float>(4, 300.0f);
            pe->snd.Wr<float>(8, 3000.0f);
            pe->snd.Wr<float>(0xC, 1.0f);
        }
        r.pimg = &pe->snd;
        break;
    case 0x258AC8: SetPositionKind(pe, 0); r.pimg = &pe->b; base = 0x10; break;   // the point (+0x20: EMITB +0x30)
    case 0x258B38: SetPositionKind(pe, 2); r.pimg = &pe->b; base = 0x10; break;   // the surface's oid
    case 0x258B00: SetPositionKind(pe, 1); r.fBox = true; break;                  // *(EMITB +0x30): the box
    case 0x258E60: case 0x258EC8:
    {
        const int n = pfnGet == 0x258E60 ? 2 : 1;
        SetKind0Payload(pe, n);
        base = pe->b.I(0xC0) == n ? 0xF0 : 0xFFFFFFFF;
        r.pimg = &pe->b;
        break;
    }
    case 0x258C60: case 0x258CC8: case 0x258D30:
    {
        const int n = pfnGet == 0x258C60 ? 2 : pfnGet == 0x258CC8 ? 3 : 8;
        SetKind1Payload(pe, n);
        base = pe->b.I(0xC0) == n ? 0xF0 : 0xFFFFFFFF;
        r.pimg = &pe->b;
        break;
    }
    default:
        r.pimg = nullptr;
        break;
    }
    if (base == 0xFFFFFFFF) r.pimg = nullptr;
    r.off = base + c18;
    return r;
}

// orig: 0x0013E818's field writes (by OTYP: 0 a byte, 1 a float, 10 two, 2 / 9 / 11 three, 5 / 6 a word, enums /
// oids a word)
void WriteOptionValue(EMITTERN* pe, const FIELDREF& r, int k, const OPTVAL& v)
{
    auto put = [&](uint32_t off, auto val) {
        if (r.fBox)
        {
            if (off < 0x20) std::memcpy(reinterpret_cast<uint8_t*>(pe->abox.data()) + off, &val, sizeof(val));
        }
        else if (r.pimg) r.pimg->Wr(off, val);
    };
    switch (k < 24 ? k : 24)
    {
    case 0: put(r.off, uint8_t(v.n)); break;
    case 1: put(r.off, v.ag[0]); break;
    case 10: put(r.off, v.ag[0]); put(r.off + 4, v.ag[1]); break;
    case 2: case 9: case 11: for (int i = 0; i < 3; ++i) put(r.off + uint32_t(4 * i), v.ag[size_t(i)]); break;
    case 3: case 12: for (int i = 0; i < 4; ++i) put(r.off + uint32_t(4 * i), v.ag[size_t(i)]); break;
    default: put(r.off, int32_t(v.n)); break;
    }
}

void ApplyEmitterOptions(EMITTERN* pe)
{
    for (const OPT& opt : pe->plo->aopt)
    {
        if (opt.aval.empty()) continue;
        const uint32_t va = VA_AEOPID + uint32_t(opt.eopid) * 0x28;
        const int k = int(g_disc.U32Elf(va));
        const uint32_t grf = g_disc.U32Elf(va + 4);
        const uint32_t c18 = g_disc.U32Elf(va + 0x18);
        const OPTVAL& v = opt.aval[0];
        if (grf & 0x401) continue;   // CRef functions (26: the post-load list; 478..480 / 489)
        if (grf & 0x90)
        {
            const uint32_t pfnGet = (grf & 0x2000) ? g_disc.U32Elf(va + 0x24) : 0;
            const FIELDREF r = FieldOfGetter(pe, pfnGet, c18);
            WriteOptionValue(pe, r, k, v);
            continue;
        }
        // a setter (grf 0x100: the PMF at +0x18, its function +0x1C)
        const uint32_t pfnSet = PfnEmitterOfExpl(g_disc.U32Elf(va + ((grf & 0x100) ? 0x1C : 0x14)));
        switch (pfnSet)
        {
        case 0x15A240:   // SetEmitterParticleCount (kind still -1 at load: the count)
            if (pe->e.I(0xF0) == -1) pe->e.Wr<int32_t>(0xF4, v.n);
            else if (pe->e.I(0xF0) == 0)
            {
                if (v.n == 0) pe->e.Wr<float>(0x148, FLT_BIG);
                else if (v.n == -1) pe->e.Wr<float>(0x148, -1.0f);
                else pe->e.Wr<int32_t>(0xF4, v.n);
            }
            pe->e.Wr<int32_t>(0xE0, 1);
            break;
        case 0x15A2B0: pe->e.Wr<int32_t>(0x114, v.n); pe->e.Wr<int32_t>(0xE0, 1); break;   // SetEmitterAutoPause
        case 0x2589A8:   // the particle kind: 0 / 1 make their blocks
            if (v.n == 0) OffKind0(pe, true);
            else if (v.n == 1) OffKind1(pe, true);
            break;
        case 0x258A00: SetPositionKind(pe, v.n); break;
        case 0x258B70: SetKind1Payload(pe, v.n); break;
        case 0x258D98: SetKind0Payload(pe, v.n); break;
        case 0xC:        // option 13 (vt slot 42: the cull radius +0x38, the LO's sRadius)
            pe->e.Wr<float>(0x38, v.ag[0]);
            pe->plo->sRadius = v.ag[0];
            break;
        default:
            break;
        }
    }
}

// orig: 0x00159118 InitEmitb + 0x0015CAD8 (the default EMITO / EMITV, 0x2DE5B0 / 0x2DE5F0)
void InitEmitb(IMG& b)
{
    std::fill(b.ab.begin(), b.ab.end(), 0);
    b.Wr<int32_t>(4, -1);
    b.Wr<int32_t>(0, 1);
    // 0x2DE5B0 (0x40 bytes at +0x10): +0x10 kind 0, +0x20 -1 (no source), +0x30 (0, 0, 0, 0), +0x40 (1, 0, 0, 0)
    b.Wr<int32_t>(0x20, -1);
    b.Wr<float>(0x40, 1.0f);
    // 0x2DE5F0 (0x70 bytes at +0x50)
    b.Wr<float>(0x50, 750.0f); b.Wr<float>(0x54, 750.0f); b.Wr<float>(0x58, -1.0f); b.Wr<float>(0x5C, 1.0f);
    b.Wr<float>(0x68, -1500.0f);
    b.Wr<float>(0x78, 1.0f);   // (0x2A2DC0 at +0x70: (0, 0, 1))
    b.Wr<int32_t>(0x84, 200);
    b.Wr<float>(0x88, 1.0f); b.Wr<float>(0x8C, 1.0f);
    b.Wr<float>(0xB0, 1.0f); b.Wr<float>(0xB8, -1.0f);
}

// orig: 0x001591B0 InitEmitter
void InitEmitterImage(IMG& e)
{
    e.Wr<int32_t>(0x118, -1);
    e.Wr<int32_t>(0xF4, -1);
    e.Wr<int32_t>(0xF0, -1);
    e.Wr<float>(0xF8, 10.0f);
    e.Wr<float>(0x148, -1.0f);
    e.Wr<float>(0xFC, 10.0f);
}

void PauseEmitter(EMITTERN* pe, float dt) { pe->e.Wr<float>(0x148, g_clock.t + dt); }
void PauseEmitterIndefinite(EMITTERN* pe) { pe->e.Wr<float>(0x148, FLT_BIG); }
// orig: 0x0015A170 FPausedEmitter
bool FPausedEmitter(const EMITTERN* pe)
{
    if (g_clock.t < pe->e.F(0x148)) return true;
    const int k = pe->e.I(0xF0);
    if (k < 2 && k >= 0) return pe->e.I(0xF4) == 0;
    return false;
}

// orig: 0x001394C8 (the particle system's settings from the EMITB) with 0x139370 (the texture) and 0x15BF40
void SetPsysFromEmitb(PSYS* pp, const EMITTERN* pe)
{
    const IMG& b = pe->b;
    const int k = b.I(0xC0);
    pp->kDraw = k;
    pp->gFlying = k == 1 ? b.F(0xF0) : 1.0f;
    pp->argba.clear();
    if (!pe->argbaColor.empty())
    {
        const size_t n = pe->argbaColor.size();
        pp->argba.resize(n);
        for (size_t i = 0; i < n; ++i)
            for (int j = 0; j < 4; ++j) pp->argba[n - i - 1][size_t(j)] = uint8_t((((pe->argbaColor[i] >> (8 * j)) & 0xFF) + 1) >> 1);
    }
    pp->fColorPairs = b.U(0xCC) != 0;
    pp->pshd = nullptr;
    if (g_pfnPcShdOfLo && (pe->plo || pe->ploMember)) pp->pshd = g_pfnPcShdOfLo(pe->plo ? pe->plo : pe->ploMember, b.I(0xD0));
    if (!pp->pshd) pp->pshd = PshdOfId(b.I(0xD0));
    // 0x139370: the draw table by the record's flag, its colour once, the textures (a power of 2 below 32)
    pp->fAdditive = pp->pshd && (pp->pshd->grfshd & 1);
    if (pp->argba.empty())
    {
        std::array<uint8_t, 4> c{};
        const uint32_t u = pp->pshd ? pp->pshd->argba[0] : 0x80808080u;
        for (int j = 0; j < 4; ++j) c[size_t(j)] = uint8_t((((u >> (8 * j)) & 0xFF) + 1) >> 1);
        pp->argba.push_back(c);
    }
    int c = 1;
    if (pp->pshd && !pp->pshd->atex.empty()) c = std::max<int>(1, int(pp->pshd->atex[0].apbmp.size()));
    int n = 1;
    for (int v = c >> 1; v; v >>= 1) n <<= 1;
    if (n < c) n <<= 1;
    while (!(n < 0x20)) n >>= 1;
    const int cExtra = c < n ? n - c : 0;
    pp->aitex.clear();
    for (int i = 0; i < c && int(pp->aitex.size()) < n; ++i)
    {
        pp->aitex.push_back(i);
        if (i < cExtra) pp->aitex.push_back(i);
    }
    pp->aitex.resize(size_t(n), 0);
    const float life = b.F(0x8C);
    std::fill(std::begin(pp->clqTex), std::end(pp->clqTex), 0.0f);
    if (n >= 2)
    {
        const float d = b.F(0xD8);
        pp->clqTex[1] = float(n) / (d == 0.0f ? life : d);
    }
    const float srcAlpha[3] = {b.F(0xB0), b.F(0xB4), b.F(0xB8)}, srcScale[3] = {b.F(0xE0), b.F(0xE4), b.F(0xE8)};
    PrescaleClq(srcAlpha, pp->clqAlpha, 1.0f / life, 0.0f);
    PrescaleClq(srcScale, pp->clqScale, 1.0f / life, 0.0f);
    // the size bound: the scale's largest magnitude over [0, life]
    if (pp->clqScale[1] == 0.0f && pp->clqScale[2] == 0.0f) pp->sSize = std::fabs(pp->clqScale[0]);
    else
    {
        auto at = [&](float t) { return pp->clqScale[0] + t * (pp->clqScale[1] + t * pp->clqScale[2]); };
        float a = std::fabs(pp->clqScale[0]), cEnd = std::fabs(at(life)), bMid = 0;
        if (pp->clqScale[2] != 0.0f)
        {
            const float tv = -pp->clqScale[1] / (pp->clqScale[2] + pp->clqScale[2]);
            if (0.0f < tv && tv < life) bMid = std::fabs(at(tv));
        }
        pp->sSize = std::max({a, bMid, cEnd});
    }
    pp->clqColor[0] = 0.0f;
    pp->clqColor[1] = 1.0f / life;
    pp->clqColor[2] = 0.0f;
    pp->kUpdate = b.I(0xC4);
    if (pp->kUpdate == 0)
    {
        const glm::vec3 a = b.V(0x60);
        if (std::fabs(a.x) < 0.0001f && std::fabs(a.y) < 0.0001f && std::fabs(a.z) < 0.0001f && b.F(0x58) == 0.0f &&
            b.F(0x80) == 0.0f)
            pp->kUpdate = 1;
    }
    // 0x15BF40: the motion parameters
    pp->accel = b.V(0x60);
    pp->axisSpin = NormalizeOr(b.V(0x70), glm::vec3(0, 0, 1));
    pp->gSpin = b.F(0x80);
    pp->gDrag = b.F(0x58);
}

// ------------------------------------------------------------ emission
// orig: 0x0015AE38 CalculateEmitvx
struct EMITVX { int cPerRing; float dTilt, dPan, radTilt0, radPan0; };
EMITVX CalculateEmitvx(int cParticlePerRing, float tiltLo, float tiltHi, int cParticle)
{
    EMITVX vx;
    vx.cPerRing = cParticlePerRing > 0 ? std::min(cParticlePerRing, cParticle) : cParticle;
    vx.cPerRing = std::max(vx.cPerRing, 1);
    const int cRing = (cParticle - 1 + vx.cPerRing) / vx.cPerRing;
    const float g = RadNormalize(tiltHi - tiltLo);
    vx.dPan = 2.0f * PI / float(vx.cPerRing);
    vx.dTilt = g / float(std::max(cRing, 1));
    vx.radTilt0 = tiltLo + vx.dTilt * 0.5f;
    vx.radPan0 = GRandInRange(0.0f, 2.0f * PI);
    return vx;
}

// orig: 0x0015AF08 ChooseEmitVelocity
glm::vec3 ChooseEmitVelocity(const EMITVX& vx, float uRandom, float rSvz, float svLo, float svHi, const glm::vec3& normal, int i)
{
    const int iRing = i / vx.cPerRing, iPan = i - iRing * vx.cPerRing;
    float radTilt = vx.radTilt0 + float(iRing) * vx.dTilt;
    float radPan = vx.radPan0 + float(iPan) * vx.dPan;
    if (uRandom != 0.0f)
    {
        radTilt += uRandom * GRandInRange(-0.5f, 0.5f) * vx.dTilt;
        radPan += uRandom * GRandInRange(-0.5f, 0.5f) * vx.dPan;
    }
    radTilt = RadNormalize(radTilt + PI * 0.5f);
    radPan = RadNormalize(radPan);
    glm::vec3 v = SetVectorSphere(radPan, radTilt, GRandInRange(svLo, svHi));
    v.z *= rSvz;
    glm::vec3 a0, a1;
    // (a zero first vector left every particle of an emitter whose normal is the y axis without a velocity: Ep6's
    // train-barrier bolts 41381 all lay along x; N25)
    BuildOrthonormalVectors(normal, glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), a0, a1);  // D_002A2DA0 / D_002A2DB0 (audit E6)
    return a0 * v.x + a1 * v.y + normal * v.z;
}

// orig: 0x0015A808 ChooseEmitPosition (kinds 0 a point, 1 a box, 2 a surface (TODO), 3 a mesh)
void ChooseEmitPosition(EMITTERN* pe, int i, int c, glm::vec3& pos, glm::vec3& normal)
{
    const IMG& b = pe->b;
    const int kPos = b.I(0x10), kVel = b.I(0x14);
    bool fNormalize = false, fVelocity = true;
    if (kPos == 0) pos = b.V(0x30);
    else if (kPos == 1)
    {
        const glm::vec4 &lo = pe->abox[0], &hi = pe->abox[1];
        pos = glm::vec3(GRandInRange(lo.x, hi.x), GRandInRange(lo.y, hi.y), GRandInRange(lo.z, hi.z));
    }
    else if (kPos == 2)
    {
        // a point on the surface's curve (the PATH +0x30: 0x1E6F10 at a distance in this particle's share)
        const CRV* pcrv = pe->s.aplo[1] ? PcrvFromLo(pe->s.aplo[1]) : nullptr;
        if (!pcrv || pcrv->as.empty())
        {
            pos = glm::vec3(0.0f);
            return;
        }
        const float d = pcrv->as.back() / float(c);
        const float lo = float(i) * d;
        const float g = GRandInRange(lo, lo + d);
        glm::vec3 dir(0.0f);
        EvalCrvS(pcrv, g, &pos, &dir);
        if (kVel == 2)
        {
            normal = dir;
            fVelocity = false;
            fNormalize = true;
        }
        else if (kVel == 3)
        {
            normal = glm::cross(dir, b.V(0x40));
            fVelocity = false;
            fNormalize = true;
        }
    }
    else if (kPos == 3 && pe->pmesh && !pe->pmesh->atri.empty())
    {
        float sTotal = 0;
        for (const auto& t : pe->pmesh->atri) sTotal += t.sArea;
        float a = GRandInRange(0.0f, 1.0f) * sTotal;
        size_t it = 0;
        a -= pe->pmesh->atri[0].sArea;
        for (size_t k = 1; k < pe->pmesh->atri.size() && a > 0.0f; ++k) { a -= pe->pmesh->atri[k].sArea; it = k; }
        const auto& tri = pe->pmesh->atri[it];
        auto P = [&](int j) {
            const int ip = tri.aipos[j];
            return ip >= 0 && size_t(ip) < pe->pmesh->apos.size() ? pe->pmesh->apos[size_t(ip)] : glm::vec3(0.0f);
        };
        const float u = GRandInRange(0.0f, 1.0f);
        float w = 1.0f - u;
        const float v = GRandInRange(0.0f, w);
        w -= v;
        pos = P(0) * u + P(1) * v + P(2) * w;
        if (kVel == 5)
        {
            const glm::vec3 n = glm::cross(P(1) - P(0), P(2) - P(0));   // (the vertex normals: the face's)
            normal = n;
            fVelocity = false;
            fNormalize = true;
        }
    }
    if (fVelocity)
    {
        fNormalize = true;
        if (kVel == 1) normal = pos - b.V(0x40);
        else if (kVel == 0) normal = b.V(0x40);
        else if (kVel == 4)
        {
            // relative to the camera, in the source's frame (MulTMat(source matrix, ...)): ChooseEmitParticles turns it
            // by the source's matrix after (natively it was turned twice; audit E10)
            const LO* ploSrc = pe->s.aplo[0];
            const glm::mat4 m = ploSrc ? MatWorldOfLo(ploSrc) : pe->s.aplo[1] ? glm::mat4(1.0f) : MatWorldOfEmitter(pe);
            normal = glm::transpose(glm::mat3(m)) * (MatParticleCamera() * b.V(0x40));
        }
        else normal = glm::vec3(1.0f, 0.0f, 0.0f);  // D_002A2DA0 (was 0: frozen particles; audit E6)
    }
    if (!fNormalize) return;
    normal = NormalizeOr(normal, glm::vec3(1.0f, 0.0f, 0.0f));  // (below 1e-4: D_002A2DA0)
    pos += normal * GRandInRange(b.F(0x24), b.F(0x28));
}

PSYS* PpsysNew(EMITTERN* pe)
{
    auto pp = std::make_unique<PSYS>();
    SetPsysFromEmitb(pp.get(), pe);
    // slot 51 0x139900 with the emitter: +0xE0 = it (its +0x38 radius)
    pp->pemitterPar = pe;
    s_apsys.push_back(std::move(pp));
    return s_apsys.back().get();
}

// ------------------------------------------------------------ RIPs (module 0x1CDCD0..0x1D1370)
// A RIP: a particle object drawing a model (the EMITB's object +0xC8) at its own position / orientation, with
// size and alpha polynomials of its age (0x1CF338). Kinds by vtable (0x1CE6D0); the worlds use 3 (oriented along
// their velocity, 0x1CF618), 8 (shrapnel, 0x1CFAB8 / 0x1CFC88), 5 and 2.
struct RIPN
{
    int kind = 3;                   // +0
    float tBirth = 0, life = 1;     // +0x14 / +0x18
    const LO* pmodel = nullptr;     // +0x1C
    float size[3] = {};             // +0x20
    float alpha[3] = {1.0f, 1.6666669f, -2.666667f};   // +0x30 (0x2C1F50)
    glm::mat3 ori{1.0f};            // +0x40 (columns: the original's rows)
    glm::vec3 pos{0.0f}, posPrev{0.0f};   // +0x70 / +0x80
    glm::vec3 vel{0.0f};            // +0x90
    glm::vec3 accel{0.0f}, axisSpin{0, 0, 1};   // +0xA0 (0x15BF40)
    float gSpin = 0, gDrag = 0;
    glm::vec3 w{0.0f};              // +0xD0 the spin
    float wDamp = 0;                // +0xE4
    const LO* ploTouch = nullptr;   // +0xE0
    uint32_t uF0 = 0;               // +0xF0 (kind 3: the second object; 8: the lift)
    float gF0 = 0;
    uint32_t fFaceCamera = 0;       // +0xF4 (kind 3)
    // kind 9 (vtable 0x2D75A8: 0x1D04E8 / 0x1D05A0 / 0x1D0800 / 0x1D0630): it flies in the camera's screen space
    glm::vec3 posCm{0.0f};          // +0x100 (screen x, screen y, depth: 0x14F308)
    glm::vec3 posGoal{0.0f}, velGoal{0.0f};   // its definition +0xF0's +0x0 / +0x10
    glm::vec3 axisSpin9{0, 0, 1};   // the definition's +0x40
    std::function<void()> pfnArrive;   // the definition's +0x60 (a member function) on +0xF4 with +0xF8
    const LO* ploFollow = nullptr;  // kind 4: +0xF0 the object it stays at (0x1D0158)
    LO* ploTrail = nullptr;         // kind 2: +0xF0 the explosion it leaves behind (0x1D1108)
    float gTrailRate = 0, gTrail = 0;   // kind 2: +0xF4 its rate per second, +0xF8 the fraction carried
    const LO* plo2 = nullptr;       // kind 3: +0xF0, kind 1: +0xF4: the EMITS's +0xC (the decal / ripple model)
    float agShrapnel[3] = {};       // kind 8: +0xF0 radius (also the draw lift), +0xF4 elasticity, +0xF8 friction
    XF* pxfSpace = nullptr;         // kind 10: +0xF0 the record of the entry it sticks to (null: the world)
    bool fRemoved = false;          // (RemoveRip 0x1CE980 during a walk: taken out after it)
};
struct RIPG
{
    int mode = 0;                   // +0x150 (0: the shared group SW +0x406C; 1: it collides, 0x1CE2E8)
    float g158 = 0;                 // +0x158 (EMITB +0xD4)
    std::vector<RIPN> arip;         // +0x15C
    // mode 1: its attachment's box (0x1CDF88 -> slot 49 0x1CE170: +0x170 lo, +0x180 hi, +0x190 center, +0x1A0 radius)
    bool fBox = false;
    glm::vec3 posMin{0.0f}, posMax{0.0f}, posCenter{0.0f};
    float sRadius = 0.0f;
};
std::vector<std::unique_ptr<RIPG>> s_aripg;
WORLD_TABLE(s_aripg);   // (PC: moved aside while a kept world loads, worldglobals.h)
RIPG* s_pripgShared = nullptr;
WORLD_VALUE(s_pripgShared);   // (PC: moved aside while a kept world loads, worldglobals.h)

// orig: 0x001CDCD0 (a new ripple group; mode 0: the shared one)
RIPG* PripgNew(int mode)
{
    if (mode == 0 && s_pripgShared) return s_pripgShared;
    s_aripg.push_back(std::make_unique<RIPG>());
    RIPG* pg = s_aripg.back().get();
    pg->mode = mode;
    if (mode == 0) s_pripgShared = pg;
    return pg;
}

// orig: 0x0011B830 BuildLookAtMatrix (rows: the direction, left = up x dir, up again)
glm::mat3 LookAt(const glm::vec3& dir, const glm::vec3& up)
{
    const glm::vec3 x = NormalizeOr(dir, glm::vec3(1, 0, 0));
    glm::vec3 y = glm::cross(up, x);
    if (glm::dot(y, y) < 1e-12f) y = glm::cross(glm::vec3(0, 1, 0), x);
    y = glm::normalize(y);
    return glm::mat3(x, y, glm::cross(x, y));
}

// orig: 0x0015B1B0 (c particles as RIPs of the emitter's group)
void EmitParticleObjects(EMITTERN* pe, int c, const glm::vec3* apos, const glm::vec3* avel, const float* at0, const float* at1)
{
    const IMG& b = pe->b;
    if (!pe->pripg)
    {
        pe->pripg = PripgNew(b.I(0xC4));
        pe->pripg->g158 = b.F(0xD4);   // SetRipgEmitb
    }
    RIPG* pg = pe->pripg;
    const int kind = b.I(0xC0);
    const LO* psrc = pe->s.aplo[0];
    const glm::mat3 oriSrc = psrc ? glm::mat3(MatWorldOfLo(psrc)) : glm::mat3(1.0f);
    for (int i = 0; i < c; ++i)
    {
        RIPN r;
        r.kind = kind;
        // slot 1 0x1CE920 InitRip (pos, n, g)
        r.pos = r.posPrev = apos[i];
        r.ploTouch = pe->s.aplo[4];
        r.size[0] = GRandInRange(b.F(0xD8), b.F(0xDC));
        if (b.F(0xE4) != 0.0f || b.F(0xE8) != 0.0f) { r.size[1] = b.F(0xE4); r.size[2] = b.F(0xE8); }
        r.vel = avel[i];
        r.tBirth = at0[i];
        r.life = at1[i] - at0[i];
        r.alpha[0] = b.F(0xB0); r.alpha[1] = b.F(0xB4); r.alpha[2] = b.F(0xB8);
        r.accel = b.V(0x60);
        r.axisSpin = NormalizeOr(b.V(0x70), glm::vec3(0, 0, 1));
        r.gSpin = b.F(0x80);
        r.gDrag = b.F(0x58);
        r.pmodel = pe->s.aplo[2];
        if (psrc) r.ori = oriSrc;
        // slot 2 (the kind's parameters from the EMITB)
        if (kind == 3)
        {
            // 0x1CF5D0: +0xF0 the EMITS's +0xC (its decal, 0x1CF968), +0xF4 the EMITB's +0xF0
            r.plo2 = pe->s.aplo[3];
            r.fFaceCamera = b.U(0xF0);
        }
        else if (kind == 1) r.plo2 = pe->s.aplo[3];   // 0x1D0CE8: +0xF4 the EMITS's +0xC (its ripple, 0x1D0D28)
        else if (kind == 8)
        {
            // 0x1CFAB8 PostShrapnelEmit: a life of 0.75..1.25 times, a spin from the speed (0x2C1F80, limited, x 10)
            r.life *= GRandInRange(0.75f, 1.25f);
            const float v = glm::length(r.vel);
            const float f = std::clamp(0.0f + v * (0.001f + v * 0.0f), 0.0f, 1.0f) * 10.0f;
            for (int k = 0; k < 3; ++k) r.w[k] = GRandInRange(-f, f);
            r.gF0 = b.F(0xF0);
            for (int k = 0; k < 3; ++k) r.agShrapnel[k] = b.F(0xF0 + 4 * k);   // (the EMITB's +0xF0 vector)
        }
        else if (kind == 2)
        {
            // 0x1D1108: an explosion trail: the object of the EMITB's +0xF0 found near the emitter's (kind 4), its
            // rate +0xF4 (natively these RIPs only flew; audit 2026-11 E7)
            r.ploTrail = PloFindOid(b.I(0xF0), pe->plo);
            r.gTrailRate = b.F(0xF4);
        }
        else if (kind == 5)
        {
            // 0x1D08D8: a random tilt (the vector (-1..1, -1..1, 0.1..0.9 + 0.1) as a rotation, 0x11B7E0)
            const glm::vec3 vt(GRandInRange(-1.0f, 1.0f), GRandInRange(-1.0f, 1.0f), GRandInRange(0.1f, 0.9f) + 0.1f);
            r.ori = LookAt(vt, glm::vec3(0, 0, 1));  // 0x11B7E0(vt): x along the tilt, z up (not x fixed)
        }
        if (std::getenv("SLY2N_RIPPRINT") && i == 0)
            std::fprintf(stderr, "RIP group mode %d g158 %.1f accel %.1f %.1f %.1f drag %.3f\n", b.I(0xC4), b.F(0xD4), r.accel.x, r.accel.y, r.accel.z, r.gDrag);
        if (std::getenv("SLY2N_RIPPRINT"))
            std::fprintf(stderr, "RIP e %d t %.3f at %.0f %.0f %.0f kind %d size %.3f %.3f %.3f (D8 %.3f DC %.3f E4 %.3f E8 %.3f) model %d src %d face %u vel %.1f %.1f %.1f spin %.2f axis %.2f %.2f %.2f ori x %.2f %.2f %.2f z %.2f %.2f %.2f\n", pe->plo ? pe->plo->oid : -1, g_clock.t, r.pos.x, r.pos.y, r.pos.z, r.kind,
                         r.size[0], r.size[1], r.size[2], b.F(0xD8), b.F(0xDC), b.F(0xE4), b.F(0xE8),
                         r.pmodel ? r.pmodel->oid : -1, psrc ? psrc->oid : -1, r.fFaceCamera, r.vel.x, r.vel.y, r.vel.z, r.gSpin,
                         r.axisSpin.x, r.axisSpin.y, r.axisSpin.z, r.ori[0].x, r.ori[0].y, r.ori[0].z, r.ori[2].x, r.ori[2].y, r.ori[2].z);
        if (std::getenv("SLY2N_RIPPRINT") && r.pmodel && FCidDerivedFrom(r.pmodel->Cid(), 0x30))
        {
            const ALO* pm = static_cast<const ALO*>(r.pmodel);
            const XF* po = pm->apxfRef[2];
            std::fprintf(stderr, "RIP emitter %d (instance %d ch %d) chain:", pe->plo ? pe->plo->oid : -1, pe->plo ? pe->plo->oidInstance : -1, pe->plo ? pe->plo->ichannel : -9);
            for (const LO* q = pe->plo ? pe->plo->paloParent : nullptr; q; q = q->paloParent) std::fprintf(stderr, " %d(%d)", q->oid, q->oidInstance);
            std::fprintf(stderr, " | model %p instance %d; all:", (const void*)pm, pm->oidInstance);
            for (const auto& up : g_psw->aploAll)
                if (up->oid == pm->oid || up->oidInstance == pm->oid) std::fprintf(stderr, " %p(%d %d ch %d)", (void*)up.get(), up->oid, up->oidInstance, up->ichannel);
            for (size_t iwe = 0; iwe < g_psw->awe.size(); ++iwe)
                for (size_t j = 0; j < g_psw->awe[iwe].aplo.size(); ++j)
                    if (g_psw->awe[iwe].aplo[j]->oid == pm->oid || g_psw->awe[iwe].aplo[j] == pe->plo)
                        std::fprintf(stderr, " [we %zu %s #%zu oid %d loaded %d shared %d]", iwe, g_psw->awe[iwe].StrName().c_str(), j, g_psw->awe[iwe].aplo[j]->oid,
                                     g_psw->awe[iwe].cloLoaded, g_psw->awe[iwe].x5C);
            std::fprintf(stderr, "\n");
            std::fprintf(stderr, "RIP model %d obj record %s local t %.3f %.3f %.3f world t %.3f %.3f %.3f root world t %.3f %.3f %.3f\n", pm->oid,
                         po ? "yes" : "no", po ? po->mat[3].x : 0.0f, po ? po->mat[3].y : 0.0f, po ? po->mat[3].z : 0.0f,
                         po ? po->matWorld[3].x : 0.0f, po ? po->matWorld[3].y : 0.0f, po ? po->matWorld[3].z : 0.0f,
                         pm->axf.empty() ? 0.0f : pm->axf[0]->matWorld[3].x, pm->axf.empty() ? 0.0f : pm->axf[0]->matWorld[3].y,
                         pm->axf.empty() ? 0.0f : pm->axf[0]->matWorld[3].z);
        }
        pg->arip.push_back(r);
    }
}

// orig: 0x0014F308 (a world point in the camera's screen space: x right and y up over the screen ranges CM +0x40 /
// +0x44, divided by the depth, z the depth)
glm::vec3 PosCmFromWorld(const glm::vec3& pos)
{
    const glm::vec3 l = glm::transpose(g_cm.mat) * (pos - g_cm.pos);  // (0x11C318: the camera matrix inverted)
    glm::vec3 r(-l.y / g_cm.xScreenRange, l.z / g_cm.yScreenRange, l.x);
    if (l.x > 1e-4f)
    {
        r.x /= l.x;
        r.y /= l.x;
    }
    return r;
}
// orig: 0x0014F268 (back: the screen point at its depth in the world)
glm::vec3 PosWorldFromCm(const glm::vec3& v)
{
    glm::vec3 r(v.z, -v.x, v.y);
    if (v.z > 1e-4f)
    {
        r.y = -v.x * v.z;
        r.z = v.y * v.z;
    }
    r.y *= g_cm.xScreenRange;
    r.z *= g_cm.yScreenRange;
    return g_cm.mat * r + g_cm.pos;
}

// orig: 0x001D05A0 (kind 9's motion: the Hermite curve 0x117120 from its screen point and velocity to the goal's over
// the time left (at least a step), then its world point (+0x70, the previous +0x80))
void MoveRipHud(RIPN& r, float t, float dt)
{
    const float dtLeft = std::max(r.tBirth + r.life - t, dt);
    glm::vec4 pos, vel;
    EvalHermiteVector(glm::vec4(r.posCm, 0.0f), glm::vec4(r.vel, 0.0f), glm::vec4(r.posGoal, 0.0f), glm::vec4(r.velGoal, 0.0f), &pos, &vel, nullptr,
                      dtLeft, dt, 1.0f);
    r.posCm = glm::vec3(pos);
    r.vel = glm::vec3(vel);
    r.posPrev = r.pos;
    r.pos = PosWorldFromCm(r.posCm);
    if (std::getenv("SLY2N_RIPPRINT"))
        std::printf("rip 9: t %.3f screen (%.3f %.3f %.1f) world (%.0f %.0f %.0f)\n", t - r.tBirth, r.posCm.x, r.posCm.y, r.posCm.z, r.pos.x, r.pos.y, r.pos.z);
}

// orig: 0x001CF0A0 (a RIP's motion: its spin turns it; moving fast enough, its velocity through its motion matrix)
void MoveRip(RIPN& r, float dt)
{
    if (glm::dot(r.w, r.w) >= 0.0004f)
    {
        r.w *= r.wDamp * dt + 1.0f;
        const float len = glm::length(r.w);
        if (0.0001f < len)
        {
            const glm::mat3 rot = glm::mat3(glm::rotate(glm::mat4(1.0f), len * dt, r.w / len));
            r.ori = rot * r.ori;
        }
    }
    r.posPrev = r.pos;
    if (glm::dot(r.vel, r.vel) < 4.0f && glm::dot(r.accel, r.accel) < 25.0f) return;
    glm::mat4 s(1.0f);
    s[0][0] = s[1][1] = s[2][2] = r.gDrag;
    s[3] = glm::vec4(r.accel, 1.0f);
    glm::mat4 rot(1.0f);
    if (r.gSpin != 0.0f) rot = glm::rotate(glm::mat4(1.0f), r.gSpin * dt, r.axisSpin);
    const glm::mat4 m = rot + (s * rot) * dt;
    r.vel = glm::vec3(m * glm::vec4(r.vel, 1.0f));
    r.pos += r.vel * dt;
}

// orig: 0x001D09A0 (kind 5's motion, e.g. the Snooze Bomb's Zs): the basis and the velocity turned about the basis's
// row 1 by clamp(-0.01 (v . row 0), -10, 10) radians a second (D_002C2010 / D_002C2020); the speed along the old row 0
// relaxed toward 200 (D_002C2028, rate D_002C202C -1), the rest damped by the drag; then 0x1CF0A0 without the drag
void MoveRipFlutter(RIPN& r, float dt)
{
    const glm::vec3 row0 = r.ori[0], axis = r.ori[1];
    const float g = glm::dot(r.vel, row0);
    const float t = std::clamp(0.0f + g * (-0.01f + g * 0.0f), -10.0f, 10.0f);
    const glm::mat3 rot = MatRotateAxis(t * dt, axis);   // 0x11B500
    r.vel = rot * r.vel;
    r.ori = rot * r.ori;
    const float drag = r.gDrag;
    r.gDrag = 0.0f;
    glm::vec3 along = row0 * glm::dot(r.vel, row0);      // 0x11E290: along / across the old row 0
    const glm::vec3 across = r.vel - along;
    const float len = glm::length(along);
    along = len < 0.0001f ? glm::vec3(1, 0, 0) : along / len;
    const float gAlong = 200.0f + (-1.0f * dt + 1.0f) * (len - 200.0f);
    const float gAcross = drag * dt + 1.0f;
    r.vel = along * gAlong + across * gAcross;
    MoveRip(r, dt);
    r.gDrag = drag;
}

// ------------------------------------------------------------ RIP hits (0x1CE1C8 / 0x1CE2E8 / slot 8 / 0x1CF268)
// (audit 2026-11 "Open" item 1: natively RIPs never collided, never stuck as decals and never died in their touch
// object)
namespace
{
std::vector<RIPN> s_aripPending;   // RIPs made during the hit walk (kept apart from the vectors being walked)
bool FRipDebug()
{
    static const bool s_f = std::getenv("SLY2N_RIPHIT") != nullptr;
    return s_f;
}

float GRipPoly(const float* ag, float u) { return ag[0] + u * (ag[1] + u * ag[2]); }

// orig: 0x001CEAA8 ForceRipFade (the rest of its life cut to dt, keeping the fraction of it lived)
void ForceRipFade(RIPN& r, float dt)
{
    const float t = r.life - (g_clock.t - r.tBirth);
    if (dt < t)
    {
        const float u = t / r.life;
        r.life = dt / u;
        r.tBirth = g_clock.t - (1.0f - u) * r.life;
    }
}

// orig: 0x0011EE08's fraction (the point of the segment a-b nearest p; a zero-length segment: 1, as the EE's 0 / 0)
float TNearestSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 d = b - a;
    const float dd = glm::dot(d, d);
    if (dd == 0.0f) return 1.0f;
    return std::clamp(glm::dot(p - a, d) / dd, 0.0f, 1.0f);
}

// orig: 0x001CEB08 (RIP vt slot 8, the default: a bounce off the entry at pos with the normal). Kind 8 uses its own
// radius / elasticity / friction (+0xF0..+0xF8), the others 100 / 0.7 / 0.8; the radius scales with the size
// polynomial. In water (its owner of class 0x59) the velocity goes halfway to the water's (0x228C18), the rest as an
// acceleration over 0.5 s, and it fades in 0.5 s (D_002C1F60 / D_002C1F64). Else an impulse at the sphere's point
// -r n: the normal impulse (elasticity), the friction one along n - mu t, mixed by the point of the segment between the
// two tangential velocities nearest zero (0x11EE08), spin with inertia 2.5 / max(r, 10)^2; an upward speed below 200
// is raised to 200 with a 0.25 s fade (D_002C1F68 / D_002C1F6C); it is pushed out of the surface by 1.
bool FBounceRip(RIPN& r, const SOE* psoe, const glm::vec3& pos, const glm::vec3& normal)
{
    float radius = 100.0f, elas = 0.7f, mu = 0.8f;
    if (r.kind == 8)
    {
        radius = r.agShrapnel[0];
        elas = r.agShrapnel[1];
        mu = r.agShrapnel[2];
    }
    radius *= GRipPoly(r.size, (g_clock.t - r.tBirth) / r.life);
    ALO* powner = psoe->palo;
    if (powner && FCidDerivedFrom(powner->Cid(), 0x59))
    {
        const glm::vec3 vWater = VelWaterAtPoint(powner, pos);
        const glm::vec3 v = r.vel + (vWater - r.vel) * 0.5f;
        r.vel = v;
        r.accel = (vWater - v) * (1.0f / 0.5f);
        ForceRipFade(r, 0.5f);
        if (FRipDebug()) std::fprintf(stderr, "RIPHIT water kind %d t %.3f at %.0f %.0f %.0f owner %d\n", r.kind, g_clock.t, pos.x, pos.y, pos.z, powner->oid);
        return true;
    }
    const glm::vec3 offset = normal * -radius;
    const glm::vec3 torque1 = glm::cross(offset, normal);
    const glm::vec3 vOther = VelXfPoint(psoe->pxf, nullptr, pos);
    radius = std::max(radius, 10.0f);
    const float invI = 2.5f / (radius * radius);
    const glm::vec3 rel = r.vel + glm::cross(r.w, offset) - vOther;
    const float vn = glm::dot(rel, normal);
    // the normal impulse
    const glm::vec3 resp1 = normal + glm::cross(torque1, offset) * invI;
    const float impulse1 = vn * (-1.0f - elas) / glm::dot(resp1, normal);
    const glm::vec3 vAfter1 = rel + resp1 * impulse1;
    const glm::vec3 tan1 = vAfter1 - normal * glm::dot(vAfter1, normal);
    const float lenTan = std::sqrt(glm::dot(tan1, tan1));
    const glm::vec3 dirTan = lenTan < 1e-4f ? glm::vec3(1, 0, 0) : tan1 * (1.0f / lenTan);
    // the friction impulse
    const glm::vec3 axis = normal + dirTan * -mu;
    const glm::vec3 torque2 = glm::cross(offset, axis);
    const glm::vec3 resp2 = axis + glm::cross(torque2, offset) * invI;
    const float impulse2 = vn * (-1.0f - elas) / glm::dot(resp2, normal);
    const glm::vec3 vAfter2 = rel + resp2 * impulse2;
    const glm::vec3 tan2 = vAfter2 - normal * glm::dot(vAfter2, normal);
    const float u = TNearestSegment(glm::vec3(0.0f), tan1, tan2);
    const float g1 = (1.0f - u) * impulse1, g2 = u * impulse2;
    r.vel += normal * g1 + axis * g2;
    r.w += (torque1 * g1 + torque2 * g2) * invI;
    if (r.vel.z < 200.0f)
    {
        r.vel.z = 200.0f;
        ForceRipFade(r, 0.25f);
    }
    const float d = glm::dot(pos - r.pos, normal);
    r.pos += normal * (std::max(d, 0.0f) + 1.0f);
    if (FRipDebug())
        std::fprintf(stderr, "RIPHIT bounce kind %d t %.3f at %.0f %.0f %.0f n %.2f %.2f %.2f owner %d vel %.0f %.0f %.0f\n", r.kind, g_clock.t, pos.x, pos.y,
                     pos.z, normal.x, normal.y, normal.z, powner ? powner->oid : -1, r.vel.x, r.vel.y, r.vel.z);
    return true;
}

// orig: 0x001CF6B0 (a decal: a kind-10 RIP of the shared group with the model pvisual stuck to the entry's record at
// pos, facing dir, turned randomly about it; its size the hitting RIP's times 0.8..1.4, shrinking to 0 over 5 s; on a
// wall (|dir . z| < 0.7) it slides down at 50 / s; raised by the hitting group's +0x158). Its orientation is the world
// one, its position the record's local one, as in the original.
RIPN* PripDecal(const RIPN& r, const RIPG* pg, const LO* pvisual, XF* pxfSpace, const glm::vec3& pos, const glm::vec3& dir)
{
    if (!pvisual) return nullptr;
    RIPN d;
    d.kind = 10;
    const glm::vec3 local = PosConvertXfs(nullptr, pxfSpace, pos);
    float size = GRipPoly(r.size, (g_clock.t - r.tBirth) / r.life);
    size *= GRandInRange(0.8f, 1.4f);
    // slot 1 0x1CE920 (local, 0, size)
    d.tBirth = g_clock.t;
    d.pos = d.posPrev = local;
    d.size[0] = size;
    d.pmodel = pvisual;
    d.life = 5.0f;
    d.size[2] = -size;
    d.pxfSpace = pxfSpace;
    // 0x11E2D8(dir, x, y): row 0 x cross dir (y cross dir when short), row 1 dir cross that; row 2 dir
    glm::vec3 x = glm::cross(glm::vec3(1, 0, 0), dir);
    if (glm::dot(x, x) < 1e-4f) x = glm::cross(glm::vec3(0, 1, 0), dir);
    const glm::vec3 y = glm::cross(dir, x);
    const glm::mat3 frame(x / std::sqrt(glm::dot(x, x)), y / std::sqrt(glm::dot(y, y)), dir);
    const float rad = GRandInRange(0.0f, 6.28318548f);
    d.ori = frame * BuildEulerMatrix(glm::vec3(0.0f, 0.0f, rad));
    if (std::fabs(dir.z) < 0.7f)
    {
        const glm::vec3 flat(dir.x, dir.y, 0.0f);
        d.vel = BuildRotateVectorsMatrix(flat, dir) * glm::vec3(0.0f, 0.0f, -50.0f);
    }
    d.pos.z += pg ? pg->g158 : 0.0f;
    if (FRipDebug())
        std::fprintf(stderr, "RIPHIT decal from kind %d t %.3f at %.0f %.0f %.0f dir %.2f %.2f %.2f model %d size %.1f\n", r.kind, g_clock.t, pos.x, pos.y, pos.z,
                     dir.x, dir.y, dir.z, pvisual->oid, size);
    s_aripPending.push_back(std::move(d));
    return &s_aripPending.back();
}

// RIP vt slot 8: the hit of its segment with an entry at pos (the entry's normal there); true stops its other hits
bool FRipHit(RIPG* pg, RIPN& r, const SOE* psoe, const glm::vec3& pos, const glm::vec3& normal)
{
    ALO* powner = psoe->palo;
    if (FRipDebug())
        std::fprintf(stderr, "RIPHIT hit kind %d t %.3f at %.0f %.0f %.0f owner %d cid 0x%x sensor %d model %d second %d\n", r.kind, g_clock.t, pos.x, pos.y, pos.z,
                     powner ? powner->oid : -1, powner ? unsigned(powner->Cid()) : 0u, int(psoe->f48), r.pmodel ? r.pmodel->oid : -1, r.plo2 ? r.plo2->oid : -1);
    switch (r.kind)
    {
    case 3:
        // 0x1CF968: not into actors (D_002C1F70 {0x32}); a decal of its +0xF0 unless water; removed
        if (powner && FCidDerivedFrom(powner->Cid(), 0x32)) return false;
        if (!powner || !FCidDerivedFrom(powner->Cid(), 0x59)) PripDecal(r, pg, r.plo2, psoe->pxf, pos, normal);
        r.fRemoved = true;
        return true;
    case 5:
    {
        // 0x1D0BF8: not into class 0x33; a decal of its own model, its orientation into the owner's frame (0x1A7050);
        // removed
        if (powner && FCidDerivedFrom(powner->Cid(), 0x33)) return false;
        if (RIPN* pd = PripDecal(r, pg, r.pmodel, psoe->pxf, pos, normal); pd && powner)
            pd->ori = InverseMatrix3(glm::mat3(MatWorldOfAlo(powner))) * pd->ori;
        r.fRemoved = true;
        return true;
    }
    case 8:
    {
        // 0x1CFBE0: through actors (the entry's attachment's root of class 0x32); removed in water; else the bounce
        const OXA* poxa = powner ? Poxa(powner) : nullptr;
        const ALO* proot = poxa ? poxa->pso : powner;
        if (proot && FCidDerivedFrom(proot->Cid(), 0x32)) return false;
        if (proot && FCidDerivedFrom(proot->Cid(), 0x59))
        {
            r.fRemoved = true;
            return true;
        }
        return FBounceRip(r, psoe, pos, normal);
    }
    default: return FBounceRip(r, psoe, pos, normal);
    }
}

// orig: 0x001CDF88's box (the RIPs' two points, lowered by the group's +0x158 (both corners, as the original), its
// center and radius)
void UpdateRipgBox(RIPG* pg)
{
    glm::vec3 lo(FLT_BIG), hi(-FLT_BIG);
    for (const RIPN& r : pg->arip)
    {
        if (r.fRemoved) continue;
        lo = glm::min(lo, glm::min(r.pos, r.posPrev));
        hi = glm::max(hi, glm::max(r.pos, r.posPrev));
    }
    if (pg->g158 != 0.0f)
    {
        lo -= glm::vec3(pg->g158);
        hi -= glm::vec3(pg->g158);
    }
    pg->posMin = lo;
    pg->posMax = hi;
    pg->posCenter = lo * 0.5f + hi * 0.5f;
    const glm::vec3 d = pg->posCenter - hi;
    pg->sRadius = std::sqrt(glm::dot(d, d));
    pg->fBox = !pg->arip.empty();
}

// orig: 0x001CE2E8 (a mode-1 group's RIPs against the entries of the attachments its box overlaps (the sweep's pairs,
// 0x133D08: both overlap kinds 0, since the group is never still): not those marked 4; sensors only of water (cid 0x59);
// a tree pruned by the group's sphere (0x118A88, at most min(0x1555, +0x6C) nodes; skipped unless mixed), else a
// sphere entry; each born RIP's segment +0x80 -> +0x70 clipped to it (0x14C6D8, at most 4 pieces), the pieces not
// starting inside given to its slot 8 until one takes it)
void CollideRipg(RIPG* pg)
{
    if (pg->mode != 1 || !pg->fBox) return;
    const float t = g_clock.t;
    if (FRipDebug())
    {
        static float s_tLast = -10.0f;
        if (t - s_tLast >= 1.0f || t < s_tLast)
        {
            s_tLast = t;
            int cover = 0;
            for (OXA* poxa : g_physw.apoxa)
                if (poxa && poxa->pso && poxa->posMax.x > pg->posMin.x && pg->posMax.x > poxa->posMin.x && poxa->posMax.y > pg->posMin.y &&
                    pg->posMax.y > poxa->posMin.y && poxa->posMax.z > pg->posMin.z && pg->posMax.z > poxa->posMin.z)
                    ++cover;
            std::fprintf(stderr, "RIPHIT group t %.2f rips %zu kind %d box (%.0f %.0f %.0f)..(%.0f %.0f %.0f) overlaps %d of %zu\n", t, pg->arip.size(),
                         pg->arip.empty() ? -1 : pg->arip[0].kind, pg->posMin.x, pg->posMin.y, pg->posMin.z, pg->posMax.x, pg->posMax.y, pg->posMax.z,
                         cover, g_physw.apoxa.size());
        }
    }
    for (OXA* poxa : g_physw.apoxa)
    {
        if (!poxa || !poxa->pso) continue;
        bool fOverlap = true;
        for (int i = 0; i < 3 && fOverlap; i++)
            if (poxa->posMax[i] <= pg->posMin[i] || pg->posMax[i] <= poxa->posMin[i]) fOverlap = false;
        if (!fOverlap || (poxa->grf & 4)) continue;
        const bool fWater = FCidDerivedFrom(poxa->pso->Cid(), 0x59);
        const std::vector<SOE*> apsoe = poxa->apsoe;
        for (SOE* psoe : apsoe)
        {
            if (!psoe || (psoe->f48 && !fWater)) continue;
            std::vector<BSPN> absp;
            int iroot = -1;
            if (!psoe->f49)
            {
                if (psoe->bsp.abspn.empty()) continue;
                int cnodeLeft = std::min(0x1555, int(psoe->bsp.x4));
                if (ClipBspPlane(BSPREF(psoe->bsp, psoe->geomWorld), 0, INT32_MAX, pg->posCenter, pg->sRadius, &cnodeLeft, &absp) != 2) continue;
                iroot = int(absp.size()) - 1;
            }
            for (size_t i = 0; i < pg->arip.size(); ++i)
            {
                RIPN& r = pg->arip[i];
                if (r.fRemoved || t <= r.tBirth) continue;
                LSG alsg[4];
                const int c = iroot >= 0 ? ClipSegmentBsp(BSPREF(absp, psoe->geomWorld), iroot, INT32_MAX, r.posPrev, r.pos, 4, alsg)
                                         : ClipSegmentToSphere(PosOf(psoe->pxf ? psoe->pxf->matWorld : glm::mat4(1.0f)), r.posPrev, r.pos, 4, alsg, psoe->sRadius);
                for (int k = 0; k < c; ++k)
                    if (alsg[k].t0 != 0.0f && FRipHit(pg, r, psoe, alsg[k].pos0, glm::vec3(alsg[k].plane0))) break;
            }
        }
    }
}

// orig: 0x001D0D28 (kind 1's slot 7: touched, and with a ripple model (+0xF4): a kind-7 RIP of it where its segment
// enters the touch object's first entry it crosses (0x14C6A0, else at its position): size 40 growing by 30, 0.75 s);
// then TouchRip
void TouchRipKind1(RIPN& r, bool fTouch)
{
    if (fTouch && r.plo2)
    {
        glm::vec3 pos = r.pos;
        if (r.ploTouch && FCidDerivedFrom(r.ploTouch->Cid(), 0x30))
            for (const SOE& e : static_cast<const ALO*>(r.ploTouch)->asoe)
            {
                LSG lsg;
                if (ClipSegmentEntry(&e, r.posPrev, r.pos, 1, &lsg))
                {
                    pos = lsg.pos0;
                    break;
                }
            }
        RIPN d;
        d.kind = 7;
        d.tBirth = g_clock.t;
        d.pos = d.posPrev = pos;
        d.life = 0.75f;
        d.size[0] = 40.0f;
        d.size[1] = 30.0f;
        d.pmodel = r.plo2;
        if (FRipDebug()) std::fprintf(stderr, "RIPHIT drip ripple t %.3f at %.0f %.0f %.0f model %d\n", g_clock.t, pos.x, pos.y, pos.z, r.plo2->oid);
        s_aripPending.push_back(std::move(d));
    }
    if (fTouch) r.fRemoved = true;   // 0x1CEA88 TouchRip
}

// orig: 0x001CF268 (RIP vt slot 5: expired, removed; in a group that does not collide, its touch object (+0xE0, with a
// mass state, in the world) containing its position (vt slot 73) touches it (slot 7: TouchRip removes it))
void UpdateRipLife(const RIPG* pg, RIPN& r)
{
    if (g_clock.t - r.tBirth > r.life)
    {
        r.fRemoved = true;
        return;
    }
    if (pg->mode == 1 || !r.ploTouch || !FCidDerivedFrom(r.ploTouch->Cid(), 0x30)) return;
    ALO* palo = static_cast<ALO*>(const_cast<LO*>(r.ploTouch));
    if (!Psost(palo)) return;
    const bool f = palo->fInWorld && FAloContainsPoint(palo, r.pos);
    if (r.kind == 1) TouchRipKind1(r, f);
    else if (r.kind != 0 && f) r.fRemoved = true;   // 0x1CEA88 (kind 0's 0x1D10E8: no world uses it)
}
}  // namespace

// Opt-in scene lookup observer: IDs, generations and current-entry membership only.
static void TraceSceneLookup(const char* cache, const SCENE& scene, const ALO* palo,
                             const SCENEGLOBSET* psgs, uint64_t generation, int ripKind)
{
    static const bool enabled = std::getenv("SLY2N_SCENECACHE") != nullptr;
    if (!enabled || !psgs || generation == s_sceneLookupGeneration) return;
    static uint64_t reportedGeneration = 0;
    static unsigned reportedCount = 0;
    if (reportedGeneration != s_sceneLookupGeneration)
    {
        reportedGeneration = s_sceneLookupGeneration;
        reportedCount = 0;
    }
    if (reportedCount++ >= 8) return;
    bool member = false;
    for (const SCENEGLOBSET& entry : scene.aglobset)
        if (&entry == psgs) { member = true; break; }
    if (generation != s_sceneLookupGeneration || !member)
        std::fprintf(stderr, "SCENECACHE lookup cache %s world %u current %llu cached %llu entries %zu model %d ripkind %d member %d\n",
                     cache, g_psw ? g_psw->serial : 0,
                     static_cast<unsigned long long>(s_sceneLookupGeneration),
                     static_cast<unsigned long long>(generation), scene.aglobset.size(), palo ? palo->oid : -1,
                     ripKind, int(member));
}

// orig: 0x1686AC..0x168794 (RenderGlob): whole-object bounds under an external draw matrix.
static void SetExtraDrawBounds(EXTRADRAW& x, const ALO* palo, const glm::mat4& external)
{
    const glm::mat4 object = palo->apxfRef[2] ? palo->apxfRef[2]->matWorld : glm::mat4(1.0f);
    const GLOBSET& gs = *palo->pglobset;
    x.fBounds = true;
    x.posBounds = glm::vec3(external * (object * glm::vec4(gs.posCenter, 1.0f)));
    x.sBoundsRadius = gs.sRadius * std::sqrt(std::max({glm::dot(glm::vec3(external[0]), glm::vec3(external[0])),
                                                     glm::dot(glm::vec3(external[1]), glm::vec3(external[1])),
                                                     glm::dot(glm::vec3(external[2]), glm::vec3(external[2]))}));
    x.sMRD = palo->sRadius;
}

// orig: 0x001CF338 / 0x001CF618 / 0x001CF518 / 0x001CFC88 (a RIP's model at its matrix, its size and alpha
// polynomials of its age): for RenderScene, the model's globs at the RIP's matrix.
void CollectRipDraws(const SCENE& scene, std::vector<EXTRADRAW>& aextra)
{
    static const SCENE* s_pscene = nullptr;
    static size_t s_cgs = 0;
    static uint64_t s_generation = 0;
    static std::map<const GLOBSET*, const SCENEGLOBSET*> s_mpgs;
    static std::map<const ALO*, const SCENEGLOBSET*> s_mpalogs;   // (a placement with its own lit copy: by its owner)
    if (s_pscene != &scene || s_cgs != scene.aglobset.size() || s_generation != s_sceneLookupGeneration)
    {
        s_pscene = &scene;
        s_cgs = scene.aglobset.size();
        s_generation = s_sceneLookupGeneration;
        if (std::getenv("SLY2N_SCENECACHE"))
            std::fprintf(stderr, "SCENECACHE rebuild cache %s world %u generation %llu entries %zu\n",
                         "rip", g_psw ? g_psw->serial : 0,
                         static_cast<unsigned long long>(s_generation), s_cgs);
        s_mpgs.clear();
        s_mpalogs.clear();
        for (const SCENEGLOBSET& sgs : scene.aglobset) s_mpgs.emplace(sgs.pglobset.get(), &sgs);
    }
    auto psgsOfAlo = [&](const ALO* palo) -> const SCENEGLOBSET* {
        if (auto ia = s_mpalogs.find(palo); ia != s_mpalogs.end()) return ia->second;
        for (const SCENEGLOBSET& sgs : scene.aglobset)
            for (const XF* pxf : sgs.apxfOwner)
                if (pxf && pxf->palo == palo) return s_mpalogs[palo] = &sgs;
        return s_mpalogs[palo] = nullptr;
    };
    const float t = g_clock.t;
    int c = 0;
    for (const auto& pg : s_aripg)
        for (const RIPN& r : pg->arip)
        {
            if (t < r.tBirth || !r.pmodel || r.life <= 0.0f || !FCidDerivedFrom(r.pmodel->Cid(), 0x30)) continue;
            const ALO* pmodel = static_cast<const ALO*>(r.pmodel);
            if (!pmodel->pglobset) continue;
            auto it = s_mpgs.find(pmodel->pglobset.get());
            const SCENEGLOBSET* psgs = it != s_mpgs.end() ? it->second : nullptr;
            if (!psgs) psgs = psgsOfAlo(pmodel);
            TraceSceneLookup("rip", scene, pmodel, psgs, s_generation, r.kind);
            if (!psgs) continue;
            const float u = (t - r.tBirth) / r.life;
            const float alpha = std::clamp(r.alpha[0] + u * (r.alpha[1] + u * r.alpha[2]), 0.0f, 1.0f);   // (0x2BBB60)
            const float size = r.size[0] + u * (r.size[1] + u * r.size[2]);
            if (alpha < 0.0001f && size < 0.0001f) continue;
            glm::mat3 ori = r.ori;
            glm::vec3 pos = r.pos;
            if (r.kind == 4)
            {
                // 0x1D0190: facing the camera (0x11B830 toward it, z up), turned about that axis by u radians
                // (D_002C1FF0), the size times clamp(0.75 d, 1, 5) and the alpha times clamp(0.5 + 0.5 d^2, 0.5, 1),
                // d the camera's distance / 1000 (D_002C1FB0 / D_002C1FD0); drawn with its model's own alpha 1
                const float d = glm::distance(g_cm.pos, r.pos) * 0.001f;
                const float sDist = std::clamp(d * 0.75f, 1.0f, 5.0f);
                const float aDist = std::clamp(0.5f + 0.5f * d * d, 0.5f, 1.0f);
                const float sz = (r.size[0] + u * (r.size[1] + u * r.size[2])) * sDist;
                const float al = std::clamp(r.alpha[0] + u * (r.alpha[1] + u * r.alpha[2]), 0.0f, 1.0f) * aDist;
                glm::mat4 m(LookAt(g_cm.pos - r.pos, glm::vec3(0, 0, 1)) * MatRotateAxis(u, glm::vec3(1, 0, 0)) * sz);
                m[3] = glm::vec4(r.pos, 1.0f);
                if (0.0f < al) AppendAloExtraDraws(scene, pmodel, m, al, aextra);
                ++c;
                continue;
            }
            if (r.kind == 9)
            {
                // 0x1D0630: at its screen point now; its orientation turned toward the camera's by the fraction of its
                // life (the rotation from it to the camera's, 0x11BC00 / 0x11BCF8 / 0x11B500), spun once about the
                // definition's axis, times its own
                pos = PosWorldFromCm(r.posCm);
                float rad;
                glm::vec3 axis;
                DecomposeRotateMatrixRad(g_cm.mat * glm::transpose(r.ori), &rad, &axis);
                ori = MatRotateAxis(rad * u, axis) * MatRotateAxis((u + u) * 3.14159274f, r.axisSpin9) * r.ori;
            }
            else if (r.kind == 3) ori = LookAt(r.vel, r.fFaceCamera ? -MatParticleCamera()[0] : glm::vec3(0, 0, 1));  // 0x1CF618: -CM +0x60's view row
            else if (r.kind == 10)
            {
                // 0x1CFA40: from its record's space (0x22FC48 / 0x22FD38)
                pos = PosConvertXfs(r.pxfSpace, nullptr, r.pos);
                ori = MatConvertXfs(r.pxfSpace, nullptr, r.ori);
            }
            else if (r.kind == 8) pos += glm::vec3(0, 0, 1) * r.gF0;
            // 0x11C288: the position, the rows times the size
            glm::mat4 m(ori * size);
            m[3] = glm::vec4(pos, 1.0f);
            // (0x1A5EE8 puts the model's object record (+0x60, vt slot 31) at the matrix, for every kind: the globs are
            // placed relative to it, not to the root record)
            const XF* pxfObj = pmodel->apxfRef[2];
            const glm::mat4 matAlo = pxfObj ? pxfObj->matWorld : pmodel->axf.empty() ? glm::mat4(1.0f) : pmodel->axf[0]->matWorld;
            const glm::mat4 matAloInv = glm::inverse(matAlo);
            if (static bool s_f = std::getenv("SLY2N_RIPPRINT") != nullptr; s_f && r.kind == 5)
            {
                s_f = false;
                std::printf("rip model %d obj %d root %zu: obj x %.2f %.2f %.2f y %.2f %.2f %.2f z %.2f %.2f %.2f t %.0f %.0f %.0f\n", pmodel->oid,
                            pxfObj ? 1 : 0, pmodel->axf.size(), matAlo[0].x, matAlo[0].y, matAlo[0].z, matAlo[1].x, matAlo[1].y, matAlo[1].z,
                            matAlo[2].x, matAlo[2].y, matAlo[2].z, matAlo[3].x, matAlo[3].y, matAlo[3].z);
                for (const GLOB& glob : pmodel->pglobset->aglob)
                {
                    const XF* pxf = glob.oidOwner >= 0 ? PxfFindAlo(const_cast<ALO*>(pmodel), glob.oidOwner) : nullptr;
                    const glm::mat4 mg = (pxf ? pxf->matWorld : matAlo) * (glob.fMatrix ? glob.dmat : glm::mat4(1.0f));
                    const glm::mat4 l = matAloInv * mg;
                    std::printf("   glob owner %d fMatrix %d local x %.2f %.2f %.2f y %.2f %.2f %.2f z %.2f %.2f %.2f t %.1f %.1f %.1f\n", glob.oidOwner,
                                glob.fMatrix, l[0].x, l[0].y, l[0].z, l[1].x, l[1].y, l[1].z, l[2].x, l[2].y, l[2].z, l[3].x, l[3].y, l[3].z);
                }
            }
            const GLOBSET& gs = *pmodel->pglobset;
            for (size_t i = 0; i < gs.aglob.size(); ++i)
            {
                const GLOB& glob = gs.aglob[i];
                const XF* pxf = glob.oidOwner >= 0 ? PxfFindAlo(const_cast<ALO*>(pmodel), glob.oidOwner) : nullptr;
                glm::mat4 mg = pxf ? pxf->matWorld : matAlo;
                if (glob.fMatrix) mg = mg * glob.dmat;
                EXTRADRAW x;
                x.psgs = psgs;
                x.iglob = i;
                x.mat = m * (matAloInv * mg);
                x.alpha = alpha;
                SetExtraDrawBounds(x, pmodel, m * matAloInv);
                aextra.push_back(x);
            }
            ++c;
        }
    g_particlestats.crip = c;
}

void UpdateRipgs()
{
    const float t = g_clock.t, dt = g_clock.dt;
    // the groups' slot 16 (0x1CE1C8): each born RIP's slot 5 (0x1CF268 and the kinds' own: kind 2's bursts 0x1D1160,
    // kind 9's arrival 0x1D0800), then a mode-1 group's hits (0x1CE2E8)
    std::vector<std::pair<LO*, glm::vec3>> atrail;  // (kind 2's bursts, after the walk: they may add RIPs / groups)
    for (size_t ig = 0; ig < s_aripg.size(); ++ig)
    {
        RIPG* pg = s_aripg[ig].get();
        auto& a = pg->arip;
        std::vector<std::function<void()>> apfn;
        for (RIPN& r : a)
        {
            if (r.fRemoved || t <= r.tBirth) continue;
            if (r.kind == 9)
            {
                // 0x1D0800: arrived: its callback first, then removed
                if (r.life < t - r.tBirth)
                {
                    if (r.pfnArrive) apfn.push_back(std::move(r.pfnArrive));
                    r.fRemoved = true;
                }
                continue;
            }
            UpdateRipLife(pg, r);
            if (r.kind == 2 && r.ploTrail)
            {
                // 0x1D1160: after 0x1CF268 (even when it removed it), one burst (vt slot 51, message kind 4 at the RIP's
                // position) per whole unit of rate x time
                r.gTrail += dt * r.gTrailRate;
                while (r.gTrail >= 1.0f)
                {
                    atrail.emplace_back(r.ploTrail, r.pos);
                    r.gTrail -= 1.0f;
                }
            }
        }
        CollideRipg(pg);
        a.erase(std::remove_if(a.begin(), a.end(), [&](const RIPN& r) { return r.fRemoved; }), a.end());
        for (auto& pfn : apfn) pfn();
    }
    if (!s_aripPending.empty())
    {
        RIPG* pg = PripgNew(0);   // (the decals and drip ripples: 0x1CE888(G, k, 0), the shared group)
        for (RIPN& r : s_aripPending) pg->arip.push_back(std::move(r));
        s_aripPending.clear();
    }
    // the groups' slot 17 (0x1CDF88): the motion of the born ones (slot 4), then a mode-1 group's box
    for (size_t ig = 0; ig < s_aripg.size(); ++ig)
    {
        RIPG* pg = s_aripg[ig].get();
        for (RIPN& r : pg->arip)
        {
            if (t < r.tBirth) continue;
            if (r.kind == 9) MoveRipHud(r, t, dt);
            else if (r.kind == 4)
            {
                // 0x1D0158: at its object's world point
                r.posPrev = r.pos;
                if (r.ploFollow) r.pos = PosLoWorld(r.ploFollow);
            }
            else if (r.kind == 5) MoveRipFlutter(r, dt);
            else MoveRip(r, dt);
        }
        if (pg->mode == 1) UpdateRipgBox(pg);
    }
    for (const auto& [ploT, pos] : atrail)
    {
        if (static bool s_f = std::getenv("SLY2N_RIPPRINT") != nullptr; s_f)
            std::fprintf(stderr, "RIP trail burst %d t %.3f at %.0f %.0f %.0f\n", ploT ? ploT->oid : -1, g_clock.t, pos.x, pos.y, pos.z);
        BURSTPARAMS bp;
        bp.grf = 4;
        bp.pos = pos;
        ExplodeLo(ploT, &bp);
    }
}

// ------------------------------------------------------------ emission
// orig: 0x0015B7E8 (the new particles' positions, velocities and times, in the source's frame)
void ChooseEmitParticles(EMITTERN* pe, int c, std::vector<glm::vec3>& apos, std::vector<glm::vec3>& avel,
                         std::vector<float>& at0, std::vector<float>& at1)
{
    const IMG& b = pe->b;
    apos.resize(size_t(c));
    avel.resize(size_t(c));
    at0.resize(size_t(c));
    at1.resize(size_t(c));
    const EMITVX vx = CalculateEmitvx(b.I(0x84), b.F(0x90), b.F(0x94), c);
    // the source object's frame (0x15AD48: the emitter itself without one)
    // (0x159C80 / 0x15C690: without a source or a surface the emitter itself; with a surface its world points)
    const LO* ploSrc = pe->s.aplo[0];
    const glm::mat4 m = ploSrc ? MatWorldOfLo(ploSrc) : pe->s.aplo[1] ? glm::mat4(1.0f) : MatWorldOfEmitter(pe);
    const int kFrame = b.I(0x18);
    for (int i = 0; i < c; ++i)
    {
        glm::vec3 pos(0.0f), normal(0.0f);
        ChooseEmitPosition(pe, i, c, pos, normal);
        // 0x15B0C8: velocity, birth / death, head start
        glm::vec3 vel = ChooseEmitVelocity(vx, b.F(0x88), b.F(0x5C), b.F(0x50), b.F(0x54), normal, i);
        const float tBirth = g_clock.t - GRandInRange(b.F(0xA0), b.F(0xA4));
        const float tDeath = tBirth + b.F(0x8C);
        const float g = GRandInRange(b.F(0x98), b.F(0x9C));
        if (g != 0.0f) pos += vel * g;
        if (pe->pmatFrame)
        {
            pos = glm::vec3(*pe->pmatFrame * glm::vec4(pos, 1.0f));
            vel = glm::mat3(*pe->pmatFrame) * vel;
        }
        // into the source's frame (0x1A72D0 / 0x1A7348; kind 3: the position only, 2: no velocity of its own)
        const glm::vec3 posLocal = pos;
        pos = glm::vec3(m * glm::vec4(pos, 1.0f));
        if (kFrame != 3)
        {
            vel = glm::mat3(m) * vel;
            if (kFrame != 2 && pe->matPrev[3][3] != 0.0f && g_clock.dt > 0.0f)
            {
                // 0x22D348: the source's velocity at the point (kind 1: at the particle, else at its origin)
                const glm::vec3 at = kFrame == 1 ? posLocal : glm::vec3(0.0f);
                vel += (glm::vec3(m * glm::vec4(at, 1.0f)) - glm::vec3(pe->matPrev * glm::vec4(at, 1.0f))) / g_clock.dt;
            }
        }
        apos[size_t(i)] = pos;
        avel[size_t(i)] = vel;
        at0[size_t(i)] = tBirth;
        at1[size_t(i)] = tDeath;
    }
}

EMITTERN* PemitterOf(LO* plo);

void InsertPsysParticles(EMITTERN* pe, int c, const glm::vec3* apos, const glm::vec3* avel, const float* at0, const float* at1,
                         const glm::vec3* aposEnd, const glm::vec3* avelEnd);
// orig: 0x0015BD20 (+ 0x15B9F0 the spline ends, + 0x15B428 into the particle system's blocks, or 0x15B1B0)
void EmitParticles(EMITTERN* pe, int c)
{
    if (c <= 0) return;
    const IMG& b = pe->b;
    static std::vector<glm::vec3> s_apos, s_avel, s_aposEnd, s_avelEnd;
    static std::vector<float> s_at0, s_at1, s_atx, s_aty;
    ChooseEmitParticles(pe, c, s_apos, s_avel, s_at0, s_at1);
    if (b.I(4) == 1)
    {
        EmitParticleObjects(pe, c, s_apos.data(), s_avel.data(), s_at0.data(), s_at1.data());
        return;
    }
    if (!pe->ppsys) pe->ppsys = PpsysNew(pe);
    PSYS* pp = pe->ppsys;
    // 0x15B9F0: update kind 2's ends: the target emitter's (+0xD4's) particles, in its frame, velocities reversed
    const bool fSpline = pp->kUpdate == 2;
    if (fSpline)
    {
        s_aposEnd.assign(size_t(c), glm::vec3(0.0f));
        s_avelEnd.assign(size_t(c), glm::vec3(0.0f));
        if (EMITTERN* pt = pe->s.aplo[2] ? PemitterOf(pe->s.aplo[2]) : nullptr)
        {
            ChooseEmitParticles(pt, c, s_aposEnd, s_avelEnd, s_atx, s_aty);
            for (auto& v : s_avelEnd) v = -v;
        }
    }
    InsertPsysParticles(pe, c, s_apos.data(), s_avel.data(), s_at0.data(), s_at1.data(), fSpline ? s_aposEnd.data() : nullptr,
                        fSpline ? s_avelEnd.data() : nullptr);
}

// orig: 0x0015B428 (c particles into the emitter's particle system's blocks: positions, velocities, birth / death
// times; a spline's ends when given)
void InsertPsysParticles(EMITTERN* pe, int c, const glm::vec3* apos, const glm::vec3* avel, const float* at0, const float* at1,
                         const glm::vec3* aposEnd, const glm::vec3* avelEnd)
{
    const IMG& b = pe->b;
    if (!pe->ppsys) pe->ppsys = PpsysNew(pe);
    PSYS* pp = pe->ppsys;
    const bool fSpline = aposEnd && avelEnd;
    PBLOCK* pblk = nullptr;
    size_t iblk = 0;
    for (int i = 0; i < c; ++i)
    {
        const glm::vec3 pos = apos[size_t(i)], vel = avel[size_t(i)];
        // into a block (43 each)
        while (!pblk || pblk->c >= 0x2B)
        {
            if (iblk < pp->ablk.size()) pblk = pp->ablk[iblk++].get();
            else
            {
                pp->ablk.push_back(std::make_unique<PBLOCK>());
                pblk = pp->ablk.back().get();
                iblk = pp->ablk.size();
            }
        }
        const int j = pblk->c++;
        pblk->apos[j] = glm::vec4(pos, at0[size_t(i)]);
        pblk->avel[j] = glm::vec4(vel, at1[size_t(i)]);
        if (fSpline)
        {
            pblk->aspline[j][0] = glm::vec4(aposEnd[size_t(i)], 0.0f);
            pblk->aspline[j][1] = glm::vec4(avelEnd[size_t(i)], 0.0f);
        }
        glm::vec4 x(0.0f);
        int nColor = NRandInRange(0, int(pp->argba.size()) - 1);
        if (pp->fColorPairs) nColor &= ~1;
        int nTex = 0;
        if (b.U(0xC8) & 0xFF) nTex = NRandInRange(0, int(pp->aitex.size()) - 1);
        float spin = 0, roll = 0;
        if (pp->kDraw == 2)
        {
            spin = GRandInRange(b.F(0xF8), b.F(0xFC));
            if (b.U(0xF0) == 0 && glm::dot(MatParticleCamera()[1], vel) > 0.0f) spin = -spin;   // against the camera's view
            if (b.U(0xF4) & 0xFF) roll = GRandInRange(-PI, PI);
        }
        uint32_t u;
        u = uint32_t(nColor); std::memcpy(&x.x, &u, 4);
        u = uint32_t(nTex); std::memcpy(&x.y, &u, 4);
        x.z = spin;
        x.w = roll;
        pblk->aextra[j] = x;
        ++pp->cparticle;
    }
}

// (drawdist-2: an emitter object of an inactive tree the draw distance option draws (activate.cpp
// FDrawDistanceVisualTree): run by the emitter pass on its private stream, as the emitters near only by the option)
bool s_fDdForcePrivate = false;

// orig: 0x00159C80 UpdateEmitter
void UpdateEmitter(EMITTERN* pe)
{
    // (the source's transform of this frame, for the next frame's source velocity)
    struct PREV { EMITTERN* p; ~PREV() { p->matPrev = p->s.aplo[0] ? MatWorldOfLo(p->s.aplo[0]) : MatWorldOfEmitter(p); } } prev{pe};
    if (pe->e.I(0xE0) != 0)
    {
        // 0x15A1C0 OnEmitterValuesChanged
        pe->e.Wr<int32_t>(0xE0, 0);
        if (pe->ppsys) SetPsysFromEmitb(pe->ppsys, pe);
        float g = GRandInRange(pe->e.F(0xF8), pe->e.F(0xFC));
        if (pe->e.I(0xF0) == 3) g *= 60.0f;
        pe->e.Wr<float>(0x130, g);
    }
    static const int s_oidDbg = std::getenv("SLY2_EMITDBG") ? std::atoi(std::getenv("SLY2_EMITDBG")) : -2;
    bool fDbg = pe->plo && pe->plo->oid == s_oidDbg;
    for (const EMITTERN* pm : pe->amember) if (pm->plo && pm->plo->oid == s_oidDbg) fDbg = true;
    if (fDbg)
    {
        for (const EMITTERN* pm : pe->amember)
            if (pm->plo)
            {
                const glm::vec3 p(MatWorldOfEmitter(pm)[3]);
                std::printf("  member %d r %.0f active %d parent %d inworld %d dist %.0f\n", pm->plo->oid, pm->e.F(0x38), FActive(pm->plo), pm->plo->paloParent ? pm->plo->paloParent->oid : -1,
                            pm->plo->fInWorld, glm::length(p - g_cm.pos));
            }
        const glm::vec3 pos(MatWorldOfEmitter(pe)[3]);
        std::printf("emit %d t %.2f: 118 %d members %zu parent %d dist %.0f r %.0f paused %d k %d left %d rate %.2f x %.2f psys %d\n", s_oidDbg, g_clock.t,
                    pe->e.I(0x118), pe->amember.size(), pe->plo && pe->plo->paloParent ? pe->plo->paloParent->oid : -1,
                    glm::length(pos - (s_fGameCamera ? g_cm.pos : s_posCamera)), pe->e.F(0x38), FPausedEmitter(pe), pe->e.I(0xF0), pe->e.I(0xF4),
                    pe->e.F(0x130), pe->e.F(0x13C), pe->ppsys ? pe->ppsys->cparticle : -1);
    }
    if (pe->e.I(0x118) != -1) return;
    // near enough to the camera: itself, or one of its members
    // (QoL draw distance, display.h: the original's test decides alone at 1x and below, so the game's random numbers
    // are drawn exactly as the original's (below 1x the particles are only hidden when drawn); above 1x an emitter
    // the original would leave idle runs too, on a private random stream and with its own emission counters, so the
    // shared stream the game's logic uses is untouched)
    const float rScale = g_display.drawDistance;
    // (the game's camera CM +0x90 in the game loop, which updates every frame drawn or not; the viewer's otherwise)
    const glm::vec3 posCam = s_fGameCamera ? g_cm.pos : s_posCamera;
    auto fNearAt = [&](const EMITTERN* p, float rMul) {
        // (0x159C80: the radius times CM +0x1C, max(1, rMRD / FOV): farther while the view is zoomed)
        const float r = p->e.F(0x38) * (s_fGameCamera ? g_cm.rMRDAdjust : 1.0f) * rMul;
        const glm::vec3 pos(MatWorldOfEmitter(p)[3]);
        return glm::dot(pos - posCam, pos - posCam) < r * r;
    };
    auto fNearScaled = [&](float rMul) {
        if (!pe->amember.empty())
        {
            for (const EMITTERN* pm : pe->amember)
                // (0x159C80: an active member without a parent counts as near, as the original's +0x10 == 0 test breaks)
                if (pm->plo && FActive(pm->plo) && (!pm->plo->paloParent || fNearAt(pm, rMul))) return true;
            return false;
        }
        if (pe->plo && pe->plo->paloParent) return fNearAt(pe, rMul);
        return true;
    };
    bool fPrivate = false;
    if (s_fDdForcePrivate || !fNearScaled(1.0f))
    {
        static const bool s_fOff = [] { const char* sz = std::getenv("SLY2N_DDEMIT"); return sz && sz[0] == '0'; }();   // (debug)
        if (s_fOff || !(rScale > 1.0f && (s_fDdForcePrivate || fNearScaled(rScale)))) return;
        // (not the emitters of RIPs, b +4 == 1: their pools are shared world objects)
        if (pe->b.I(4) == 1) return;
        fPrivate = true;
        if (static const char* s_sz = std::getenv("SLY2N_DDEMITSKIP"); s_sz && pe->plo && pe->plo->oid == std::atoi(s_sz)) return;   // (debug)
        if (static const char* s_sz = std::getenv("SLY2N_DDEMITONLY"); s_sz && (!pe->plo || pe->plo->oid != std::atoi(s_sz))) return;   // (debug)
        if (static const bool s_f = std::getenv("SLY2N_DDEMITPRINT") != nullptr; s_f && !pe->fPrivateInit)
            std::printf("ddemit private %d k %d b4 %d members %zu psys %p\n", pe->plo ? pe->plo->oid : -1, pe->e.I(0xF0), pe->b.I(4), pe->amember.size(), (void*)pe->ppsys);
    }
    if (!fPrivate && FPausedEmitter(pe)) return;
    // (the run-time fields a run changes: +0x130 rate, +0x138 next rate time, +0x144 fraction, +0x148 pause end, +0xF4
    // count left: private copies; the whole image is restored afterwards, so the original's run later finds them as
    // it left them)
    static constexpr uint32_t s_aoffRun[] = {0x130, 0x138, 0x144, 0x148, 0xF4};
    struct PRIVATE
    {
        EMITTERN* pe = nullptr;
        uint32_t uRand = 0;
        uint64_t cRand = 0;
        std::vector<uint8_t> abSaved;
        PSYS* ppsys = nullptr;
        PRIVATE(EMITTERN* p) : pe(p)
        {
            if (!pe) return;
            uRand = g_uRandState;
            cRand = g_cRandLibc;
            abSaved = pe->e.ab;
            if (!pe->fPrivateInit)
            {
                // (its first private run: the fields as they are now, a stream of its own)
                pe->fPrivateInit = true;
                pe->uRandPrivate = 0x9E3779B9u ^ uint32_t(pe->plo ? pe->plo->oid : int(reinterpret_cast<uintptr_t>(pe) >> 4));
                for (size_t i = 0; i < 5; ++i) pe->anRunPrivate[i] = pe->e.I(s_aoffRun[i]);
            }
            g_uRandState = pe->uRandPrivate;
            for (size_t i = 0; i < 5; ++i) pe->e.Wr<int32_t>(s_aoffRun[i], pe->anRunPrivate[i]);
            ppsys = pe->ppsys;
            if (!pe->ppsysPrivate) pe->ppsysPrivate = PpsysNew(pe);
            pe->ppsys = pe->ppsysPrivate;
        }
        ~PRIVATE()
        {
            if (!pe) return;
            pe->ppsysPrivate = pe->ppsys;
            pe->ppsys = ppsys;
            pe->uRandPrivate = g_uRandState;
            for (size_t i = 0; i < 5; ++i) pe->anRunPrivate[i] = pe->e.I(s_aoffRun[i]);
            pe->e.ab = abSaved;
            g_uRandState = uRand;
            g_cRandLibc = cRand;
        }
    } priv(fPrivate ? pe : nullptr);
    // (a private run pauses as the original's would, on its own fields)
    if (fPrivate && FPausedEmitter(pe)) return;
    int c = 0;
    const int k = pe->e.I(0xF0);
    auto take = [&](int n) {
        const int left = pe->e.I(0xF4);
        if (left >= 0)
        {
            n = std::min(n, left);
            pe->e.Wr<int32_t>(0xF4, left - n);
        }
        return n;
    };
    if (k == 1)
    {
        const int n = int(pe->e.F(0x110) * pe->e.F(0x13C));
        // (the EMITB's +4: a RIP emitter counts its group's RIPs (0x11A3C0), else the particle system's; natively the
        // RIP emitters counted 0 and emitted n every frame: audit 2026-11 E3)
        const int cur = pe->b.I(4) ? (pe->pripg ? static_cast<int>(pe->pripg->arip.size()) : 0)
                                   : (pe->ppsys ? pe->ppsys->cparticle : 0);
        c = std::max(n - cur, 0);
    }
    else if (k == 0)
    {
        float f = pe->e.F(0x144) + pe->e.F(0x130) * pe->e.F(0x13C) * g_clock.dt;
        c = int(f);
        pe->e.Wr<float>(0x144, f - float(c));
        if (c != 0) c = take(c);  // (the rate refresh below runs either way: audit E1)
    }
    else if (k == 2)
    {
        c = take(int(pe->e.F(0x130)));
        if (pe->e.U(0x114)) PauseEmitterIndefinite(pe);
        else if (GRandInRange(0.0f, 1.0f) <= pe->e.F(0x104)) PauseEmitter(pe, GRandInRange(pe->e.F(0x108), pe->e.F(0x10C)));
        pe->e.Wr<float>(0x130, GRandInRange(pe->e.F(0xF8), pe->e.F(0xFC)));
    }
    else if (k == 3)
    {
        float f = pe->e.F(0x144) + pe->e.F(0x130) * pe->e.F(0x13C) * g_clock.dt;
        c = int(f);
        pe->e.Wr<float>(0x144, f - float(c));
        if (1.0f < float(c))
        {
            c = take(c);
            bool fNewRate = true;
            if (pe->e.U(0x114)) PauseEmitterIndefinite(pe);
            else if (GRandInRange(0.0f, 1.0f) <= pe->e.F(0x104)) PauseEmitter(pe, GRandInRange(pe->e.F(0x108), pe->e.F(0x10C)));
            else fNewRate = false;
            if (fNewRate) pe->e.Wr<float>(0x130, GRandInRange(pe->e.F(0xF8), pe->e.F(0xFC)) * 60.0f);
        }
        // (c 0 or 1 here: one particle is emitted, with no count taken and no pause roll, as the original; audit E2)
    }
    if (c != 0) EmitParticles(pe, c);
    if (pe->e.F(0x138) <= g_clock.t)
    {
        pe->e.Wr<float>(0x130, GRandInRange(pe->e.F(0xF8), pe->e.F(0xFC)));
        pe->e.Wr<float>(0x138, g_clock.t + pe->e.F(0x134));
    }
    // 0x159A20 ModifyEmitterParticles: the wrap box (+0x140)
    const float h = pe->e.F(0x140);
    if (0.0001f < h && pe->b.I(4) == 0 && pe->ppsys)
    {
        glm::vec3 lo(-h), hi(h);
        if (const LO* ps = pe->s.aplo[0]) { const glm::vec3 p(MatWorldOfLo(ps)[3]); lo = p - glm::vec3(h); hi = p + glm::vec3(h); }
        const float s = h + h;
        for (auto& pb : pe->ppsys->ablk)
            for (int i = 0; i < pb->c; ++i)
                for (int a = 0; a < 3; ++a)
                {
                    float& g = pb->apos[i][a];
                    if (g < lo[a] || hi[a] < g) g = lo[a] + GModPositive(g - lo[a], s);
                }
    }
}

// orig: 0x0015BFD8 + 0x00139DC0 (the main-memory path; 0x139AB8's VU0 program computes the same)
void UpdatePsys(PSYS* pp)
{
    const float dt = g_clock.dt, t = g_clock.t;
    glm::mat4 m(1.0f);
    if (pp->kUpdate == 0)
    {
        glm::mat4 s(1.0f);
        s[0][0] = s[1][1] = s[2][2] = pp->gDrag;
        s[3] = glm::vec4(pp->accel, 1.0f);
        glm::mat4 rot(1.0f);
        if (pp->gSpin != 0.0f) rot = glm::rotate(glm::mat4(1.0f), pp->gSpin * dt, pp->axisSpin);
        // (the original's row vectors: prot + dt (prot x S))
        const glm::mat4 rs = s * rot;
        m = rot + rs * dt;
        m[3][3] = 1.0f;
    }
    glm::vec3 lo(FLT_BIG), hi(-FLT_BIG);
    pp->cparticle = 0;
    for (auto& pb : pp->ablk)
    {
        int j = 0;
        for (int i = 0; i < pb->c; ++i)
        {
            if (pb->avel[i].w <= t) continue;
            glm::vec4 vel = pb->avel[i];
            if (pp->kUpdate == 0)
            {
                const glm::vec3 v = glm::vec3(m * glm::vec4(glm::vec3(vel), 1.0f));
                vel = glm::vec4(v, vel.w);
            }
            glm::vec4 pos = pb->apos[i];
            if (pp->kUpdate == 0 || pp->kUpdate == 1) pos = glm::vec4(glm::vec3(pos) + glm::vec3(vel) * dt, pos.w);
            else if (pp->kUpdate == 2)
            {
                // 0x117120 EvalHermiteVector toward the end over the remaining time
                const float rem = vel.w - t, tt = std::min(rem, dt);
                const glm::vec3 p0(pos), v0(vel), p1(pb->aspline[i][0]), v1(pb->aspline[i][1]);
                const float u = tt / rem, w1 = 1.0f - u;
                const glm::vec3 p = p0 * (w1 * w1 * w1 + 3.0f * u * w1 * w1) + v0 * (u * w1 * w1 * rem) - v1 * (u * u * w1 * rem) +
                                    p1 * (u * u * u + 3.0f * u * u * w1);
                const float a6 = u * (6.0f - 6.0f * u) / rem, bb = 1.0f - u * (4.0f - 3.0f * u), cc = u * (3.0f * u - 2.0f);
                const glm::vec3 v = (p1 - p0) * a6 + v0 * bb + v1 * cc;
                pos = glm::vec4(p, pos.w);
                vel = glm::vec4(v, vel.w);
                pb->aspline[j][0] = pb->aspline[i][0];
                pb->aspline[j][1] = pb->aspline[i][1];
            }
            glm::vec4 x = pb->aextra[i];
            if (pp->kDraw == 2) x.w = RadNormalize(x.w + x.z * dt);
            pb->apos[j] = pos;
            pb->avel[j] = vel;
            pb->aextra[j] = x;
            lo = glm::min(lo, glm::vec3(pos));
            hi = glm::max(hi, glm::vec3(pos));
            ++j;
        }
        pb->c = j;
        pp->cparticle += j;
    }
    pp->ablk.erase(std::remove_if(pp->ablk.begin(), pp->ablk.end(), [](const auto& p) { return p->c == 0; }), pp->ablk.end());
    if (pp->cparticle > 0)
    {
        pp->posCenter = (lo + hi) * 0.5f;
        pp->sRadius = glm::length(hi - lo) * 0.5f + pp->sSize;
    }
}

EMITTERN* PemitterOf(LO* plo)
{
    auto it = s_mpemitter.find(plo);
    return it == s_mpemitter.end() ? nullptr : it->second;
}

// orig: 0x00159C80 (vt slot 16 of class 0x1D)
void UpdateEmitterLo(LO* plo)
{
    if (FOtherSw(g_psw)) InitEmittersOfSw(g_psw);
    if (EMITTERN* pe = PemitterOf(plo))
    {
        ++g_particlestats.cupdate;
        UpdateEmitter(pe);
    }
}

struct INITEMIT
{
    INITEMIT()
    {
        g_pfnExtraDraws = CollectRipDraws;
        RegisterLoUpdate(0x159C80, UpdateEmitterLo);
        // the particle systems' slot 17 (0x139DC0) in the world's pass 0x1F8DD8; the groups made at load
        RegisterSwPass(0x1F8DD8, [](SW* psw) {
            s_fGameCamera = true;
            if (FOtherSw(psw)) InitEmittersOfSw(psw);
            if (std::getenv("SLY2_EMITDBG")) { static int s_c = 0; if (s_c++ < 2) { int cg = 0; for (auto& pe : s_aemitter) cg += !pe->plo; std::printf("emitter pass: %zu emitters, %d groups\n", s_aemitter.size(), cg); } }
            for (auto& pe : s_aemitter)
                if (!pe->plo) UpdateEmitter(pe.get());   // (the groups: their own objects' updates)
            // (QoL draw distance above 1x, drawdist-2: the emitter objects of the inactive trees the option draws, which
            // the update table leaves out, run privately: their particles show as far as the trees do; the shared
            // random stream, the emitter's fields and its source's last matrix are as the original leaves them)
            if (g_display.drawDistance > 1.0f)
                for (auto& pe : s_aemitter)
                {
                    if (!pe->plo || !pe->amember.empty() || pe->b.I(4) == 1 || pe->e.I(0xE0) != 0 || FLoActive(pe->plo) || !FDrawDistanceVisualTree(pe->plo)) continue;
                    const glm::mat4 matPrev = pe->matPrev;
                    pe->matPrev = pe->matPrevPrivate;
                    s_fDdForcePrivate = true;
                    UpdateEmitter(pe.get());
                    s_fDdForcePrivate = false;
                    pe->matPrevPrivate = pe->matPrev;
                    pe->matPrev = matPrev;
                }
            for (auto& pp : s_apsys) UpdatePsys(pp.get());
            UpdateRipgs();
            // debug: SLY2_BURSTAT=frame:oid explodes an explosion object in the game loop at that frame
            static int s_cframe = 0, s_frameBurst = -1, s_oidBurst = -1;
            static const bool s_fBurst = std::getenv("SLY2_BURSTAT") && std::sscanf(std::getenv("SLY2_BURSTAT"), "%d:%d", &s_frameBurst, &s_oidBurst) == 2;
            if (s_fBurst && ++s_cframe == s_frameBurst) ExplodeOid(psw, s_oidBurst);
        });
    }
} s_initemit;

// ------------------------------------------------------------ drawing
struct PVTX
{
    glm::vec4 clip;
    glm::vec2 uv;
    float layer;
    float pad;
    glm::vec4 rgba;   // GS units
};
GLSHADER s_shParticle;
GLuint s_vao = 0, s_vbo = 0;
std::map<const SHD*, std::pair<GLuint, int>> s_mptexarray;   // a shader's frames as a texture array
WORLD_TABLE(s_mptexarray);   // (PC: moved aside while a kept world loads, worldglobals.h)
std::vector<GLuint> s_atexarrayFreed;   // (the old world's arrays: deleted at the next draw, where GL is current)

std::pair<GLuint, int> TexArrayOf(const SHD* pshd)
{
    if (!s_atexarrayFreed.empty())
    {
        glDeleteTextures(static_cast<GLsizei>(s_atexarrayFreed.size()), s_atexarrayFreed.data());
        s_atexarrayFreed.clear();
    }
    auto it = s_mptexarray.find(pshd);
    if (it != s_mptexarray.end()) return it->second;
    std::pair<GLuint, int> r{0, 0};
    if (pshd && !pshd->atex.empty() && !pshd->atex[0].apbmp.empty())
    {
        const TEX& tex = pshd->atex[0];
        const int c = int(tex.apbmp.size());
        const BMP* p0 = tex.apbmp[0];
        int dx = p0 ? p0->dx : 1, dy = p0 ? p0->dy : 1;
        // the frames' texels, and their texture mods (texmod.h: a frame's PNG at its own size; when any frame has one,
        // the array takes the first one's size, the other frames scaled to it, and gets a mip chain)
        std::vector<std::vector<uint8_t>> argba(c);
        std::vector<std::vector<uint8_t>> amod(c);
        std::vector<std::pair<int, int>> asizeMod(c, {0, 0});
        int dxMod = 0, dyMod = 0;
        for (int i = 0; i < c; ++i)
        {
            const TEXSEL sel = TexselFromShd(pshd, i, 0);
            std::string err;
            if (!(sel.pbmp && sel.pbmp->dx == dx && sel.pbmp->dy == dy && FDecodeTexture(sel.pbmp, sel.pclut, argba[i], err)))
            {
                argba[i].clear();
                continue;
            }
            if (FLoadTextureMod(argba[i], dx, dy, amod[i], &asizeMod[i].first, &asizeMod[i].second) && !dxMod)
            {
                dxMod = asizeMod[i].first;
                dyMod = asizeMod[i].second;
            }
            if (const char* sz = std::getenv("SLY2N_PARTTEXDUMP"); sz && std::strtol(sz, nullptr, 0) == pshd->id)  // (debug: the frames as PAM files, row 0 first)
                if (FILE* pf = std::fopen((std::string("parttex_") + std::to_string(pshd->id) + "_" + std::to_string(i) + ".pam").c_str(), "wb"))
                {
                    std::fprintf(pf, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", dx, dy);
                    std::fwrite(argba[i].data(), 1, argba[i].size(), pf);
                    std::fclose(pf);
                }
        }
        const bool fMod = dxMod > 0;
        if (fMod && std::getenv("SLY2N_MODSTATS"))
            std::printf("mods: particle shader %d: %d frames at %dx%d for %dx%d\n", pshd->id, c, dxMod, dyMod, dx, dy);
        const int dxSrc = dx, dySrc = dy;
        if (fMod) { dx = dxMod; dy = dyMod; }
        int clevel = 1;
        if (fMod)
            for (int d = std::max(dx, dy); d > 1; d >>= 1) ++clevel;
        glGenTextures(1, &r.first);
        glBindTexture(GL_TEXTURE_2D_ARRAY, r.first);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, clevel, GL_RGBA8, dx, dy, c);
        for (int i = 0; i < c; ++i)
        {
            const std::vector<uint8_t>* p = nullptr;
            int dxFrame = dxSrc, dyFrame = dySrc;
            if (fMod && !amod[i].empty()) { p = &amod[i]; dxFrame = asizeMod[i].first; dyFrame = asizeMod[i].second; }
            else if (!argba[i].empty()) p = &argba[i];
            if (!p) continue;
            if (dxFrame == dx && dyFrame == dy)
            {
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, dx, dy, 1, GL_RGBA, GL_UNSIGNED_BYTE, p->data());
                continue;
            }
            // (a frame of another size: scaled to the array's, nearest texel)
            std::vector<uint8_t> ab(size_t(dx) * dy * 4);
            for (int y = 0; y < dy; ++y)
                for (int x = 0; x < dx; ++x)
                    std::memcpy(&ab[(size_t(y) * dx + x) * 4], &(*p)[(size_t(y * dyFrame / dy) * dxFrame + size_t(x * dxFrame / dx)) * 4], 4);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, dx, dy, 1, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
        }
        if (clevel > 1) glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        // TEX1 0x60 (bilinear; a texture mod's mip chain trilinear), CLAMP of the record
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, clevel > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        r.second = c;
    }
    s_mptexarray[pshd] = r;
    return r;
}

float Curve(const float c[3], float age) { return c[0] + c[1] * age + c[2] * (age * age); }
float Unit(float x) { return std::clamp(x, 0.0f, 1.0f); }

// The VU1 programs (native_blip_geometry.cpp's reconstruction): per particle a quad in clip space.
void ExpandParticles(const PSYS* pp, const glm::mat4& matModel, const glm::mat4& matWorldToClip, std::vector<PVTX>& av)
{
    const float t = g_clock.t;
    const glm::vec4 forward(matWorldToClip[0][3], matWorldToClip[1][3], matWorldToClip[2][3], 0.0f);
    // the projection's x / y scales (CM +0x100 / +0x114): the clip offsets of a unit corner (the clip matrix's x / y
    // rows are the view's axes times them)
    const float sxProj = glm::length(glm::vec3(matWorldToClip[0][0], matWorldToClip[1][0], matWorldToClip[2][0]));
    const float syProj = glm::length(glm::vec3(matWorldToClip[0][1], matWorldToClip[1][1], matWorldToClip[2][1]));
    for (const auto& pb : pp->ablk)
        for (int i = 0; i < pb->c; ++i)
        {
            const glm::vec4& pc = pb->apos[i];
            const glm::vec4& vd = pb->avel[i];
            const glm::vec4& x = pb->aextra[i];
            uint32_t uColor, uTex;
            std::memcpy(&uColor, &x.x, 4);
            std::memcpy(&uTex, &x.y, 4);
            const float age = t - pc.w;
            static const float s_rDbg = std::getenv("SLY2_PARTSCALE") ? float(std::atof(std::getenv("SLY2_PARTSCALE"))) : 1.0f;
            const float scale = Curve(pp->clqScale, age) * s_rDbg;
            if ([] { static const bool f = std::getenv("SLY2_PARTDBG") != nullptr; return f; }() && i == 0)
                std::printf("psys shd 0x%X age %.2f scale %.1f alpha %.2f color n %zu rgba0 %d %d %d %d tex %zu add %d kdraw %d at %.0f %.0f %.0f\n",
                            pp->pshd ? pp->pshd->id : 0, age, scale, Unit(Curve(pp->clqAlpha, age)), pp->argba.size(), pp->argba[0][0],
                            pp->argba[0][1], pp->argba[0][2], pp->argba[0][3], pp->aitex.size(), int(pp->fAdditive), pp->kDraw, pc.x, pc.y, pc.z);
            const float alpha = Unit(Curve(pp->clqAlpha, age)), color = Unit(Curve(pp->clqColor, age));
            const float texture = Curve(pp->clqTex, age);
            const int ntab = std::max<int>(1, int(pp->aitex.size()));
            const uint32_t itab = (uTex + uint32_t(int32_t(texture))) & uint32_t(ntab - 1);
            const int layer = pp->aitex.empty() ? 0 : pp->aitex[itab];
            const size_t nColor = pp->argba.size();
            if (uColor >= nColor) continue;
            const size_t first = nColor - 1 - uColor;
            glm::vec4 rgba;
            for (int l = 0; l < 4; ++l) rgba[l] = float(pp->argba[first][size_t(l)]);
            if (pp->fColorPairs && first >= 1)
                for (int l = 0; l < 4; ++l)
                    rgba[l] = float(uint8_t(float(pp->argba[first][size_t(l)]) * (1 - color) + float(pp->argba[first - 1][size_t(l)]) * color));
            rgba.a = float(uint8_t(rgba.a * alpha));
            const glm::vec3 world = glm::vec3(matModel * glm::vec4(glm::vec3(pc), 1.0f));
            glm::vec4 clip[4];
            if (pp->kDraw == 1)
            {
                glm::vec3 motion = glm::mat3(matModel) * glm::vec3(vd) * (pp->gFlying * g_clock.dt40);
                const float diameter = scale + scale;
                const float l2 = glm::dot(motion, motion);
                if (l2 * l2 - diameter * diameter < 0.0f) motion = NormalizeOr(motion, glm::vec3(0.0f)) * diameter;
                glm::vec3 across = glm::cross(motion, glm::vec3(forward));
                across = NormalizeOr(across, glm::vec3(0.0f)) * scale;
                const glm::vec3 head = world + motion;
                const glm::vec3 apt[4] = {head + across, head - across, world + across, world - across};
                for (int k = 0; k < 4; ++k) clip[k] = matWorldToClip * glm::vec4(apt[k], 1.0f);
            }
            else
            {
                const glm::vec4 center = matWorldToClip * glm::vec4(world, 1.0f);
                std::array<glm::vec2, 4> corners{glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(-1, 1), glm::vec2(1, 1)};
                if (pp->kDraw == 2)
                {
                    float angle = x.w;
                    if (static const bool s_f = std::getenv("SLY2N_PARTNOROLL") != nullptr; s_f) angle = 0.0f;  // (debug)
                    if (angle < -PI * 0.5f)
                    {
                        angle += PI * 0.5f;
                        corners = {glm::vec2(-1, 1), glm::vec2(-1, -1), glm::vec2(1, 1), glm::vec2(1, -1)};
                    }
                    else if (angle >= PI * 0.5f)
                    {
                        angle -= PI * 0.5f;
                        corners = {glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, -1), glm::vec2(-1, 1)};
                    }
                    const float s = std::sin(angle), c = std::sqrt(std::max(0.0f, 1 - s * s));
                    for (auto& cr : corners)
                    {
                        const float cx = cr.x * scale, cy = cr.y * scale;
                        cr = glm::vec2(c * cx - s * cy, s * cx + c * cy);
                    }
                }
                else
                    for (auto& cr : corners) cr *= scale;
                // The original's clip space (CombineEyeLookAtProj 0x14D8F8: x = right, y = down, CM +0x100 / +0x114
                // positive) has y down on the screen: a corner's +y goes down, so the texture's first row (the
                // corners 2 / 3, t = 0) is at the bottom and a positive roll turns clockwise on the screen. Native's
                // clip y is up: the corner's y goes in as -y (U2: the sleep Zs were drawn upside down).
                for (int k = 0; k < 4; ++k)
                {
                    clip[k] = center;
                    clip[k].x += corners[size_t(k)].x * sxProj;
                    clip[k].y -= corners[size_t(k)].y * syProj;
                }
            }
            bool fBehind = false;
            for (const auto& v : clip) fBehind = fBehind || v.w <= 0.0f;
            if (fBehind) continue;
            // the strip's corners (0, 1, 2, 3) with STQ (0, 1) (1, 1) (0, 0) (1, 0) -> two triangles
            static const glm::vec2 s_auv[4] = {{0, 1}, {1, 1}, {0, 0}, {1, 0}};
            static const int s_ai[6] = {0, 1, 2, 2, 1, 3};
            for (int k : s_ai) av.push_back({clip[k], s_auv[k], float(layer), 0.0f, rgba});
            ++g_particlestats.cdrawn;
        }
}
}  // namespace

// (for the actors: 0x195368's particles of the player's emitter part at the interaction targets)
bool EmitLoParticlesAt(LO* plo, int c, const glm::vec3* apos, const glm::vec3* avel, const float* at0, const float* at1)
{
    EMITTERN* pe = plo ? PemitterOf(plo) : nullptr;
    if (!pe || c <= 0) return false;
    InsertPsysParticles(pe, c, apos, avel, at0, at1, nullptr, nullptr);
    return true;
}
float GEmitterLo(LO* plo, bool fEmitb, int off)
{
    EMITTERN* pe = plo ? PemitterOf(plo) : nullptr;
    if (!pe) return 0.0f;
    return fEmitb ? pe->b.F(off) : pe->e.F(off);
}


// orig: 0x002586D8 (the EMITS: the source, the surface, the kind's objects, by oid)
void ResolveEmits(EMITTERN* pe)
{
    const IMG& b = pe->b;
    pe->s = EMITS{};
    pe->s.aplo[0] = PloFindOid(b.I(0x20), pe->plo);
    // (the surface's kind 2: the second object; with it the first is cleared)
    if (b.I(0x10) == 2 && b.I(0x30) != -1)
    {
        pe->s.aplo[1] = PloFindOid(b.I(0x30), pe->plo);
        pe->s.aplo[0] = nullptr;
    }
    if (b.I(4) == 0) pe->s.aplo[2] = PloFindOid(b.I(0xD4), pe->plo);
    else if (b.I(4) == 1)
    {
        pe->s.aplo[2] = PloFindOid(b.I(0xC8), pe->plo);
        pe->s.aplo[3] = PloFindOid(b.I(0xCC), pe->plo);
        pe->s.aplo[4] = PloFindOid(b.I(0xD0), pe->plo);
    }
    if (static const char* s_sz = std::getenv("SLY2N_EMITMODEL"); s_sz)  // (debug: the emitters of a model oid)
        for (int i = 2; i < 5; ++i)
            if (pe->s.aplo[i] && pe->s.aplo[i]->oid == std::atoi(s_sz))
                std::printf("emitmodel %d: emitter %d (parent %d ch %d kind %d) -> %p ch %d\n", pe->s.aplo[i]->oid, pe->plo ? pe->plo->oid : -1,
                            pe->plo && pe->plo->paloParent ? pe->plo->paloParent->oid : -1, pe->plo ? pe->plo->ichannel : -9, b.I(4),
                            (void*)pe->s.aplo[i], pe->s.aplo[i]->ichannel);
}

// (PC addition: the scene rebuilt within a world, game.cpp: the caches of its glob sets' addresses dropped)
void InvalidateSceneLookups() { ++s_sceneLookupGeneration; }

void ResetParticles()
{
    ++s_sceneLookupGeneration;
    if (std::getenv("SLY2N_SCENECACHE"))
        std::fprintf(stderr, "SCENECACHE reset generation %llu previous_world %u\n",
                     static_cast<unsigned long long>(s_sceneLookupGeneration), s_serialSw);
    s_aemitter.clear();
    s_mpemitter.clear();
    s_apsys.clear();
    s_aripg.clear();
    s_pripgShared = nullptr;
    s_psw = nullptr;
    // (the arrays are keyed by the old world's shaders: a new world's shader may get a freed one's address)
    for (const auto& [pshd, r] : s_mptexarray)
        if (r.first) s_atexarrayFreed.push_back(r.first);
    s_mptexarray.clear();
}

// orig: 0x0015C690 (an explosion's burst: a copy of its EMITB with the overrides, emitted at once)
void EmitBurst(EMITTERN* pe, const BURSTPARAMS& bp)
{
    EMITTERN tmp;
    tmp.plo = pe->plo;
    tmp.b = pe->b;
    tmp.s = pe->s;
    tmp.abox = pe->abox;
    tmp.fBox = pe->fBox;
    tmp.pmesh = pe->pmesh;
    tmp.argbaColor = pe->argbaColor;
    IMG& b = tmp.b;
    int c = (bp.grf & 0x80) ? bp.c : int(GRandInRange(pe->e.F(0xF4), pe->e.F(0xF8)));
    if (bp.grf & 1) tmp.s.aplo[0] = bp.plo;
    if (bp.grf & 2) b.WrV(0x40, bp.dir);
    if (bp.grf & 4)
    {
        b.Wr<int32_t>(0x10, 0);
        b.WrV(0x30, bp.pos);
    }
    else if (bp.grf & 0x40) tmp.pmatFrame = &bp.matFrame;
    if ((bp.grf & 8) && b.I(4) == 0)
    {
        // the density: fewer (or more) particles, faster, bigger
        float g = bp.g8;
        if (g == 0.0f) g = bp.gC * 0.01f;
        const float fc = float(c);
        c = int(g < 1.0f ? fc * g : fc / g);
        const int cMax = int(pe->e.F(0xF8)) / 2;
        if (!(cMax < c)) c = cMax;
        const float sv = std::clamp(g, 0.5f, 2.0f);   // GLimitLm(D_002BDEC0 {0.5, 2}) (was [0, 1]: audit E4)
        b.Wr<float>(0x50, b.F(0x50) * sv);
        b.Wr<float>(0x54, b.F(0x54) * sv);
        const float gs = std::max(g, 0.5f);
        b.WrV(0xE0, b.V(0xE0) * gs);
    }
    if (b.I(4) == 1)
    {
        if (bp.grf & 0x10) { b.Wr<float>(0xD8, bp.g8); b.Wr<float>(0xDC, bp.g8); }
        if (bp.grf & 0x20) tmp.s.aplo[4] = bp.ploTouch;
    }
    const bool fGroups = pe->e.Rd<uint8_t>(0xFC) != 0;
    if (fGroups) { tmp.ppsys = pe->ppsys; tmp.pripg = pe->pripg; }
    if (!tmp.s.aplo[0] && b.I(0x10) != 2 && !(bp.grf & 0x48)) tmp.s.aplo[0] = pe->plo;
    EmitParticles(&tmp, c);
    if (tmp.ppsys) tmp.ppsys->pemitterPar = pe;
    if (fGroups) { pe->ppsys = tmp.ppsys; pe->pripg = tmp.pripg; }
    ++g_particlestats.cburst;
    // the sound (+0xF0): at the burst's place by the position kind (0 the point, 1 the box's centre, 2 / 3 the
    // source's origin here), through the frame, then from the source's space to the world's (0x1A6F00)
    if (!pe->snd.ab.empty() && pe->snd.I(0) >= 0)
    {
        glm::vec3 pos(0.0f);
        switch (b.I(0x10))
        {
        case 0: pos = b.V(0x30); break;
        case 1: pos = 0.5f * (glm::vec3(tmp.abox[0]) + glm::vec3(tmp.abox[1])); break;
        default: break;
        }
        if (tmp.pmatFrame) pos = glm::vec3(*tmp.pmatFrame * glm::vec4(pos, 1.0f));
        // (0x1A6F00(source, 0, pos): the source's vt slot 31, its object record, not its root: e.g. a pickup's)
        if (LO* psrc = const_cast<LO*>(tmp.s.aplo[0])) pos = PosJtConvert(psrc, nullptr, pos);
        if (CSnd* psnd = PsndStartSound(pe->snd.I(0), 1.0f, 0, nullptr, &pos))  // 0x1FBB90 / 0x1FB2F8(0) / 0x1FB3F0
        {
            UsePcCopyBank(psnd, pe->plo);   // (PC: a kept world's copy's explosion: its sound id is its world's, R44)
            psnd->SetFalloff(pe->snd.F(4), pe->snd.F(8));  // 0x1FB4F0
            psnd->SetVolume(pe->snd.F(0xC));               // 0x1FB1F8
            psnd->SetPitch(pe->snd.F(0x10));               // 0x1FB2C0
        }
        if ([] { static const bool f = std::getenv("SLY2_BURSTDBG") != nullptr; return f; }())
            std::printf("burst: sound %d at %.0f %.0f %.0f falloff %g %g volume %g kind %d src world %.0f %.0f %.0f\n", pe->snd.I(0), pos.x, pos.y,
                        pos.z, pe->snd.F(4), pe->snd.F(8), pe->snd.F(0xC), b.I(0x10), tmp.s.aplo[0] ? MatWorldOfLo(tmp.s.aplo[0])[3].x : 0.0f,
                        tmp.s.aplo[0] ? MatWorldOfLo(tmp.s.aplo[0])[3].y : 0.0f, tmp.s.aplo[0] ? MatWorldOfLo(tmp.s.aplo[0])[3].z : 0.0f);
    }
}

namespace
{
// the emitters (class 0x1D) and explosions (class 0x20) given, as the world's init builds them
void AddEmitters(SW* psw, const std::vector<LO*>& aploEmit, const std::vector<LO*>& aploExpl)
{
    std::vector<EMITTERN*> aemitterNew;
    for (LO* plo : aploEmit)
    {
        auto pe = std::make_unique<EMITTERN>();
        pe->plo = plo;
        InitEmitterImage(pe->e);
        InitEmitb(pe->b);
        if (const LOXEMIT* px = dynamic_cast<const LOXEMIT*>(plo))
        {
            pe->pmesh = px->pemitmesh.get();
            // 0x2584A8: the colours into EMITB +0xDC (+0xCC: their kind)
            if (!px->argba.empty())
            {
                pe->argbaColor = px->argba;
                pe->b.Wr<int32_t>(0xCC, px->nColors);
            }
        }
        // +0x38 is the LO's own radius (InitLo: 1e10, e.g. 0x1C3458 tests for it; option 13 sets it): the near test
        // below. It was 0 here unless option 13 was authored, so a parented emitter without it never emitted (Ep4
        // Lightning Action's bolt 88322: emitters 88324 / 88326 and 88328 / 88330, nothing drawn in the strikes)
        pe->e.Wr<float>(0x38, plo->sRadius);
        ApplyEmitterOptions(pe.get());
        if ([] { static const bool f = std::getenv("SLY2_PARTDBG") != nullptr; return f; }())
            if (const LOXEMIT* px = dynamic_cast<const LOXEMIT*>(plo))
            {
                const SHD* ps = PshdOfId(pe->b.I(0xD0));
                std::printf("emitter %d (parent %d %p) colours %zu (kind %d) first %08X shader 0x%X (%s) rgba %08X %08X %08X grf 0x%X frames %zu\n", plo->oid, plo->paloParent ? plo->paloParent->oid : -1, (void*)plo,
                            px->argba.size(), px->nColors, px->argba.empty() ? 0 : px->argba[0], pe->b.I(0xD0), ps && ps->id == pe->b.I(0xD0) ? "found" : "first",
                            ps ? ps->argba[0] : 0, ps ? ps->argba[1] : 0, ps ? ps->argba[2] : 0, ps ? ps->grfshd : 0,
                            ps && !ps->atex.empty() ? ps->atex[0].apbmp.size() : 0);
            }
        if (std::getenv("SLY2_EMITLIST"))
        {
            const glm::vec3 pos = glm::vec3(MatWorldOfEmitter(pe.get())[3]);
            std::printf("emitlist %d cid 0x%X inworld %d pos %.1f %.1f %.1f parent %d iupd %d grfUpd 0x%X group %d\n", plo->oid, int(plo->Cid()), plo->fInWorld, pos.x, pos.y, pos.z,
                        plo->paloParent ? plo->paloParent->oid : -1, plo->iupd, plo->grfUpd, pe->e.I(0x118));
        }
        s_mpemitter[plo] = pe.get();
        aemitterNew.push_back(pe.get());
        s_aemitter.push_back(std::move(pe));
    }
    // the explosions (0x259030: an EMITB with the same options; bursts only)
    for (LO* plo : aploExpl)
    {
        auto pe = std::make_unique<EMITTERN>();
        pe->plo = plo;
        pe->fExpl = true;
        InitEmitb(pe->b);
        if (const LOXEMIT* px = dynamic_cast<const LOXEMIT*>(plo))
        {
            pe->pmesh = px->pemitmesh.get();
            if (!px->argba.empty())
            {
                pe->argbaColor = px->argba;
                pe->b.Wr<int32_t>(0xCC, px->nColors);
            }
        }
        ApplyEmitterOptions(pe.get());
        if (const char* psz = std::getenv("SLY2_EXPLOPTS"); psz && plo->oid == std::atoi(psz))
            for (const OPT& opt : plo->aopt)
            {
                const uint32_t va = VA_AEOPID + uint32_t(opt.eopid) * 0x28;
                std::printf("explopt %d: eopid %d k %d grf 0x%X get 0x%X set 0x%X/0x%X c18 0x%X val n %d g %g %g %g\n", plo->oid, opt.eopid, g_disc.U32Elf(va),
                            g_disc.U32Elf(va + 4), g_disc.U32Elf(va + 0x24), g_disc.U32Elf(va + 0x14), g_disc.U32Elf(va + 0x1C), g_disc.U32Elf(va + 0x18),
                            opt.aval.empty() ? 0 : opt.aval[0].n, opt.aval.empty() ? 0.f : opt.aval[0].ag[0], opt.aval.empty() ? 0.f : opt.aval[0].ag[1],
                            opt.aval.empty() ? 0.f : opt.aval[0].ag[2]);
            }
        ResolveEmits(pe.get());
        s_mpemitter[plo] = pe.get();
        s_aemitter.push_back(std::move(pe));
        ++g_particlestats.cexpl;
    }
    // orig: 0x002586D8 (the EMITS: the objects by oid) + 0x00159500 PostEmitterLoad
    std::map<int32_t, EMITTERN*> mpgroup;
    for (EMITTERN* pe : aemitterNew)
    {
        IMG& b = pe->b;
        ResolveEmits(pe);
        IMG& e = pe->e;
        if (e.I(0xF0) == -1)
        {
            if (e.F(0x104) != 0.0f || e.U(0x114)) e.Wr<int32_t>(0xF0, 3);
            else if (e.F(0x110) == 0.0f) e.Wr<int32_t>(0xF0, 0);
            else e.Wr<int32_t>(0xF0, 1);
        }
        const int k = e.I(0xF0);
        if (k == 2 || k == 3)
        {
            e.Wr<float>(0x138, FLT_BIG);
            float g = GRandInRange(e.F(0xF8), e.F(0xFC));
            if (k == 3) g *= 60.0f;
            e.Wr<float>(0x130, g);
            if (e.U(0x114)) PauseEmitterIndefinite(pe);
            else if (GRandInRange(0.0f, 1.0f) <= e.F(0x104)) PauseEmitter(pe, GRandInRange(e.F(0x108), e.F(0x10C)));
        }
        else if (k == 0 || k == 1)
        {
            e.Wr<float>(0x134, 1.0f);
            e.Wr<float>(0x138, 1.0f);
            e.Wr<float>(0x130, GRandInRange(e.F(0xF8), e.F(0xFC)));
        }
        e.Wr<float>(0x13C, 1.0f);
        if (b.I(0x10) == 1)
        {
            const glm::vec4 &lo = pe->abox[0], &hi = pe->abox[1];
            if (pe->s.aplo[0])
            {
                const float dx = std::fabs(hi.x - lo.x), dy = std::fabs(hi.y - lo.y), dz = std::fabs(hi.z - lo.z);
                e.Wr<float>(0x140, std::max({dx, dy, dz}) * 0.5f);
            }
            if (e.Rd<uint8_t>(0x100) != 0)
            {
                const float z = hi.z - lo.z;
                const float g = e.F(0x110) == 0.0f ? (hi.y - lo.y) * z * 0.0001f : (hi.x - lo.x) * (hi.y - lo.y) * z * 1e-6f;
                e.Wr<float>(0x13C, g);
            }
        }
        const int32_t oid = e.I(0x118);
        if (oid == -1) continue;
        // the group of emitters (a class-0x1D object with the oid, made from this one when missing)
        EMITTERN* pg = nullptr;
        auto it = mpgroup.find(oid);
        if (it != mpgroup.end()) pg = it->second;
        else
        {
            auto png = std::make_unique<EMITTERN>();
            pg = png.get();
            InitEmitterImage(pg->e);
            pg->b = pe->b;
            pg->ploMember = pe->plo;
            pg->b.Wr<int32_t>(0, 1);
            pg->s = pe->s;
            pg->s.aplo[0] = nullptr;
            pg->b.Wr<int32_t>(0x20, -1);
            pg->abox = pe->abox;
            pg->fBox = pe->fBox;
            pg->pmesh = pe->pmesh;
            pg->argbaColor = pe->argbaColor;
            for (uint32_t off : {0xF0u, 0xF4u, 0xF8u, 0xFCu, 0x100u, 0x104u, 0x108u, 0x10Cu, 0x110u, 0x114u})
                pg->e.Wr<uint32_t>(off, e.U(off));
            pg->e.Wr<float>(0x38, e.F(0x38));
            // slot 30 with the first member's rotation (its position stays 0)
            pg->matGroup = glm::mat4(glm::mat3(MatWorldOfEmitter(pe)));
            // (PostEmitterLoad of the new group: its kind and rates as above)
            const int kg = pg->e.I(0xF0);
            if (kg == 0 || kg == 1)
            {
                pg->e.Wr<float>(0x134, 1.0f);
                pg->e.Wr<float>(0x138, 1.0f);
                pg->e.Wr<float>(0x130, GRandInRange(pg->e.F(0xF8), pg->e.F(0xFC)));
            }
            else if (kg == 2 || kg == 3)
            {
                pg->e.Wr<float>(0x138, FLT_BIG);
                pg->e.Wr<float>(0x130, GRandInRange(pg->e.F(0xF8), pg->e.F(0xFC)) * (kg == 3 ? 60.0f : 1.0f));
            }
            pg->e.Wr<float>(0x13C, e.F(0x13C));
            mpgroup[oid] = pg;
            s_aemitter.push_back(std::move(png));
            ++g_particlestats.cemitterGroup;
        }
        pe->e.Wr<int32_t>(0xE0, 0);
        pe->pgroup = pg;
        pg->amember.push_back(pe);
        // the member no longer updates (+0x40 &= ~2)
        if (pe->plo)
        {
            if (pe->plo->iupd >= 0 && size_t(pe->plo->iupd) < psw->aupd.size()) psw->aupd[size_t(pe->plo->iupd)].grf &= ~GRFUPD_Update;
            else pe->plo->grfUpd &= ~GRFUPD_Update;
        }
    }
    g_particlestats.cemitter += int(aemitterNew.size());
}
}  // namespace

void InitEmittersOfSw(SW* psw)
{
    ResetParticles();
    s_psw = psw;
    s_serialSw = psw ? psw->serial : 0;
    if (!psw) return;
    g_particlestats = PARTICLESTATS{};
    AddEmitters(psw, AploOfClass(psw, 0x1D), AploOfClass(psw, 0x20));
}

void AddEmittersOfNewLos(SW* psw, const std::vector<LO*>& aplo)
{
    if (!psw || FOtherSw(psw)) return;   // (not built yet: the world's init takes them with the rest)
    std::vector<LO*> aploEmit, aploExpl;
    for (LO* plo : aplo)
    {
        if (!plo || s_mpemitter.count(plo)) continue;
        if (FCidDerivedFrom(plo->Cid(), 0x1D)) aploEmit.push_back(plo);
        if (FCidDerivedFrom(plo->Cid(), 0x20)) aploExpl.push_back(plo);   // (as the init: both lists when both)
    }
    if (!aploEmit.empty() || !aploExpl.empty()) AddEmitters(psw, aploEmit, aploExpl);
}

void SetParticleCamera(const glm::vec3& pos, const glm::vec3& dirView, const glm::vec3& dirLeft)
{
    s_posCamera = pos;
    s_matCamera = glm::mat3(dirView, dirLeft, glm::cross(dirView, dirLeft));
}

void DumpEmitters()
{
    for (const auto& pe : s_aemitter)
    {
        const glm::vec3 p(MatWorldOfEmitter(pe.get())[3]);
        std::printf("emitter %d%s at %.0f %.0f %.0f: kind %d particles %d draw %d update %d shader 0x%X life %.2f rate %.1f..%.1f "
                    "members %zu group %d psys %d pos %d vel %d src %d parent %d dist %.0f\n", pe->plo ? pe->plo->oid : -1, pe->plo ? "" : " (group)", p.x, p.y, p.z,
                    pe->e.I(0xF0), pe->b.I(4), pe->b.I(0xC0), pe->b.I(0xC4), pe->b.I(0xD0), pe->b.F(0x8C), pe->e.F(0xF8),
                    pe->e.F(0xFC), pe->amember.size(), pe->pgroup ? 1 : 0, pe->ppsys ? pe->ppsys->cparticle : -1, pe->b.I(0x10), pe->b.I(0x14),
                    pe->b.I(0x20), pe->plo && pe->plo->paloParent ? pe->plo->paloParent->oid : -1, glm::length(p - s_posCamera));
    }
}

void StepParticles(SW* psw, int cframe, float dt)
{
    if (FOtherSw(psw)) InitEmittersOfSw(psw);
    s_fAllActive = true;
    const float tSave = g_clock.t, dtSave = g_clock.dt;
    for (int i = 0; i < cframe; ++i)
    {
        g_clock.dt = dt;
        g_clock.t += dt;
        for (auto& pe : s_aemitter)
            if (!pe->pgroup && !pe->fExpl) UpdateEmitter(pe.get());
        for (auto& pp : s_apsys) UpdatePsys(pp.get());
        UpdateRipgs();
    }
    (void)tSave;
    (void)dtSave;
}

void DrawParticles(const glm::mat4& matWorldToClip, const glm::vec3& posCamera, float aspect)
{
    (void)aspect;
    s_posCamera = posCamera;
    if (s_apsys.empty()) return;
    if (!s_shParticle.ID)
    {
        s_shParticle.Init("particle.vert", "particle.frag");
        glGenVertexArrays(1, &s_vao);
        glGenBuffers(1, &s_vbo);
        glBindVertexArray(s_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(PVTX), reinterpret_cast<void*>(offsetof(PVTX, clip)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(PVTX), reinterpret_cast<void*>(offsetof(PVTX, uv)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(PVTX), reinterpret_cast<void*>(offsetof(PVTX, layer)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(PVTX), reinterpret_cast<void*>(offsetof(PVTX, rgba)));
        glBindVertexArray(0);
    }
    g_particlestats.cdrawn = 0;
    g_particlestats.cpsys = int(s_apsys.size());
    // the view frustum (as render.cpp's)
    glm::vec4 aplane[5];
    {
        const glm::mat4 mt = glm::transpose(matWorldToClip);
        aplane[0] = mt[3] + mt[0]; aplane[1] = mt[3] - mt[0]; aplane[2] = mt[3] + mt[1]; aplane[3] = mt[3] - mt[1]; aplane[4] = mt[3] - mt[2];
        for (glm::vec4& p : aplane) p /= glm::length(glm::vec3(p));
    }
    auto fVisible = [&](const glm::vec3& c, float r) {
        for (const glm::vec4& p : aplane)
            if (glm::dot(glm::vec3(p), c) + p.w < -r) return false;
        return true;
    };
    s_shParticle.Use();
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glUniform1i(s_shParticle.Loc("tex"), 0);
    const GLint locAfail = s_shParticle.Loc("afailMode");
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);   // TEST 0x7180B: ZTST GREATER
    glEnable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
#if SLY2_FASTPATHS
    // (the Switch: the vertices and the matrices kept from one system and frame to the next, not allocated anew for
    // every particle system every frame)
    static std::vector<PVTX> av;
    static std::vector<glm::mat4> amat;
#else
    std::vector<PVTX> av;
#endif
    for (const auto& pp : s_apsys)
    {
        if (pp->cparticle <= 0) continue;
        // 0x13A1E8: once per visible member instance of the group, or once untransformed
#if SLY2_FASTPATHS
        amat.clear();
#else
        std::vector<glm::mat4> amat;
#endif
        const EMITTERN* par = pp->pemitterPar;
        // (QoL draw distance below 1x: the particles of an emitter beyond its scaled range are not drawn; it still runs
        // as the original's, UpdateEmitter)
        auto fHiddenDd = [&](const EMITTERN* p) {
            const float rScale = g_display.drawDistance;
            if (!(rScale < 1.0f) || !p || !p->plo || !p->plo->paloParent) return false;
            const glm::vec3 pos(MatWorldOfEmitter(p)[3]);
            const float r = p->e.F(0x38) * rScale;
            return glm::dot(pos - posCamera, pos - posCamera) >= r * r;
        };
        if (par && !par->amember.empty())
        {
            for (const EMITTERN* pm : par->amember)
            {
                if (!pm->plo || !FActive(pm->plo)) continue;
                if (fHiddenDd(pm)) continue;
                // (the source's position and rotation (0x1A62E8 / 0x1A6318); without one the member's position with
                // its parent's rotation (+0x10), else none: not the member's own rotation. The group already emits in
                // the first member's rotation (slot 30 at load), so drawing through the member's whole matrix turned
                // the particles by it twice: R28, Ep4's chimney smoke 68571 (tilted 112 degrees about y under the
                // spinning 68569) left its chimney sideways and swept round with the spin)
                const LO* ps = pm->s.aplo[0];
                glm::mat4 m(1.0f);
                if (ps) m = MatWorldOfLo(ps);
                else
                {
                    if (pm->plo->paloParent) m = glm::mat4(glm::mat3(MatWorldOfLo(pm->plo->paloParent)));
                    m[3] = glm::vec4(PosLoWorld(pm->plo), 1.0f);
                }
                const glm::vec3 c = glm::vec3(m * glm::vec4(pp->posCenter, 1.0f));
                if (!fVisible(c, pp->sRadius)) continue;
                amat.push_back(m);
            }
        }
        else if (fVisible(pp->posCenter, pp->sRadius) && !fHiddenDd(par)) amat.push_back(glm::mat4(1.0f));
        if (amat.empty()) continue;
        av.clear();
        for (const glm::mat4& m : amat) ExpandParticles(pp.get(), m, matWorldToClip, av);
        if (av.empty()) continue;
        const auto [tex, ctex] = TexArrayOf(pp->pshd);
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        glUniform1i(s_shParticle.Loc("fTextured"), tex ? 1 : 0);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, pp->fAdditive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(av.size() * sizeof(PVTX)), av.data(), GL_STREAM_DRAW);
        // ATE GEQUAL 0x80 with AFAIL FB_ONLY: the opaque part writes Z, the rest only colour
        glUniform1i(locAfail, 1);
        glDepthMask(GL_TRUE);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(av.size()));
        glUniform1i(locAfail, 2);
        glDepthMask(GL_FALSE);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(av.size()));
    }
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(0);
}

void ExplodeLo(LO* plo, const BURSTPARAMS* pparams)
{
    if (!plo) return;
    if (FOtherSw(g_psw)) InitEmittersOfSw(g_psw);
    static const BURSTPARAMS s_bpNone;
    const BURSTPARAMS& bp = pparams ? *pparams : s_bpNone;
    EMITTERN* pe = PemitterOf(plo);
    if (pe && pe->fExpl)
    {
        if ([] { static const bool f = std::getenv("SLY2_BURSTDBG") != nullptr; return f; }())
        {
            const LOX* px = dynamic_cast<const LOX*>(plo);
            const glm::mat4 m = MatWorldOfLo(plo);
            std::printf("burst: oid %d cid 0x%X parent %d (cid 0x%X) pxf %p pos %.0f %.0f %.0f\n", plo->oid, plo->Cid(),
                        plo->paloParent ? plo->paloParent->oid : -1, plo->paloParent ? plo->paloParent->Cid() : 0,
                        px ? (const void*)px->pxf : nullptr, m[3].x, m[3].y, m[3].z);
            std::printf("burstparams: oid %d grf 0x%X source %d touch %d frame", plo->oid, bp.grf,
                        bp.plo ? bp.plo->oid : -1, bp.ploTouch ? bp.ploTouch->oid : -1);
            for (int i = 0; i < 4; ++i)
                std::printf(" %.9g %.9g %.9g", bp.matFrame[i].x, bp.matFrame[i].y, bp.matFrame[i].z);
            std::printf("\n");
        }
        EmitBurst(pe, bp);
        return;
    }
    if ([] { static const bool f = std::getenv("SLY2_BURSTDBG") != nullptr; return f; }())
        std::printf("burst: oid %d cid 0x%X not an explosion emitter (pe %p), children %zu\n", plo->oid, plo->Cid(), (void*)pe,
                    dynamic_cast<const LOXCHILD*>(plo) ? dynamic_cast<const LOXCHILD*>(plo)->aploChild.size() : size_t(0));
    // orig: 0x0015C3C0 (a grouper's slot 51): each child bursts in its frame relative to the group. A supplied
    // frame is composed with that relative frame, and the group is the default source object.
    if (const LOXCHILD* pc = dynamic_cast<const LOXCHILD*>(plo))
    {
        BURSTPARAMS child = bp;
        child.grf |= 0x40;
        if (!(child.grf & 1))
        {
            child.plo = plo;
            child.grf |= 1;
        }
        for (LO* p : pc->aploChild)
        {
            const glm::mat4 groupWorld = MatWorldOfLo(plo), childWorld = MatWorldOfLo(p);
            cmm::MATRIX4 groupMatrix, childMatrix;
            for (int i = 0; i < 4; ++i)
            {
                groupMatrix.r[i] = groupWorld[i];
                childMatrix.r[i] = childWorld[i];
            }
            const cmm::MATRIX4 rel = cmm::MulMatrix4(cmm::InverseAffineMatrix4(groupMatrix), childMatrix);
            const glm::mat4 relative(rel.r[0], rel.r[1], rel.r[2], rel.r[3]);
            child.matFrame = (bp.grf & 0x40) ? bp.matFrame * relative : relative;
            ExplodeLo(p, &child);
        }
    }
}

void ExplodeOid(SW* psw, int32_t oid, const BURSTPARAMS* pparams)
{
    if (!psw) return;
    for (LO* plo : AploOfClass(psw, 0x20))
        if (plo->oid == oid) ExplodeLo(plo, pparams);
}

// orig: 0x001CE888(G, 9, 0) and its init 0x1D04E8 (see emitter.h)
bool FLaunchHudRip(const LO* pmodel, const glm::vec3& posWorld, const glm::mat3& matWorld, const RIPHUDDEF& def, std::function<void()> pfnArrive)
{
    if (FOtherSw(g_psw)) InitEmittersOfSw(g_psw);
    RIPG* pg = PripgNew(0);  // 0x1CDCD0(G, 0): the shared group
    RIPN r;
    r.kind = 9;
    // 0x1CE920 InitRip (pos, 0, 1): born now, life 1, size 1, the default alpha, at pos, unrotated
    r.tBirth = g_clock.t;
    r.pos = r.posPrev = posWorld;
    // 0x1D04E8: its screen point, a random screen velocity, life D_002C2000, alpha D_002BBB50, size the definition's
    r.posCm = PosCmFromWorld(posWorld);
    if (std::getenv("SLY2N_RIPPRINT"))
        std::printf("rip 9 launch: world (%.0f %.0f %.0f) cm pos (%.0f %.0f %.0f) view (%.2f %.2f %.2f) left (%.2f %.2f %.2f) ranges %.3f %.3f particle cam (%.0f %.0f %.0f)\n",
                    posWorld.x, posWorld.y, posWorld.z, g_cm.pos.x, g_cm.pos.y, g_cm.pos.z, g_cm.mat[0].x, g_cm.mat[0].y, g_cm.mat[0].z, g_cm.mat[1].x,
                    g_cm.mat[1].y, g_cm.mat[1].z, g_cm.xScreenRange, g_cm.yScreenRange, s_posCamera.x, s_posCamera.y, s_posCamera.z);
    const float vx = GRandInRange(def.velLo.x, def.velHi.x);
    const float vy = GRandInRange(def.velLo.y, def.velHi.y);
    const float vz = GRandInRange(def.velLo.z, def.velHi.z);
    r.vel = glm::vec3(vx, vy, vz);
    r.life = 0.5f;
    r.alpha[0] = 1.0f; r.alpha[1] = 0.0f; r.alpha[2] = 0.0f;
    for (int i = 0; i < 3; ++i) r.size[i] = def.size[i];
    r.posGoal = def.posGoal;
    r.velGoal = def.velGoal;
    r.axisSpin9 = def.axisSpin;
    // the caller's: the model (+0x1C), its orientation (+0x40), the callback (+0xF0 / +0xF4 / +0xF8)
    r.pmodel = pmodel;
    r.ori = matWorld;
    r.pfnArrive = std::move(pfnArrive);
    pg->arip.push_back(std::move(r));
    return true;
}

// The scene's entry of an object (by its glob set; a placement with its own lit copy by its globs' owner records)
const SCENEGLOBSET* PsgsOfAlo(const SCENE& scene, const ALO* palo)
{
    static const SCENE* s_pscene = nullptr;
    static size_t s_cgs = 0;
    static uint64_t s_generation = 0;
    static std::map<const GLOBSET*, const SCENEGLOBSET*> s_mpgs;
    static std::map<const ALO*, const SCENEGLOBSET*> s_mpalogs;
    if (s_pscene != &scene || s_cgs != scene.aglobset.size() || s_generation != s_sceneLookupGeneration)
    {
        s_pscene = &scene;
        s_cgs = scene.aglobset.size();
        s_generation = s_sceneLookupGeneration;
        if (std::getenv("SLY2N_SCENECACHE"))
            std::fprintf(stderr, "SCENECACHE rebuild cache %s world %u generation %llu entries %zu\n",
                         "alo", g_psw ? g_psw->serial : 0,
                         static_cast<unsigned long long>(s_generation), s_cgs);
        s_mpgs.clear();
        s_mpalogs.clear();
        for (const SCENEGLOBSET& sgs : scene.aglobset) s_mpgs.emplace(sgs.pglobset.get(), &sgs);
    }
    if (!palo || !palo->pglobset) return nullptr;
    if (auto it = s_mpgs.find(palo->pglobset.get()); it != s_mpgs.end())
    {
        TraceSceneLookup("alo", scene, palo, it->second, s_generation, -1);
        return it->second;
    }
    if (auto ia = s_mpalogs.find(palo); ia != s_mpalogs.end())
    {
        TraceSceneLookup("alo", scene, palo, ia->second, s_generation, -1);
        return ia->second;
    }
    for (const SCENEGLOBSET& sgs : scene.aglobset)
        for (const XF* pxf : sgs.apxfOwner)
            if (pxf && pxf->palo == palo) return s_mpalogs[palo] = &sgs;
    return s_mpalogs[palo] = nullptr;
}

// orig: 0x001A5EE8's draw (the object's hierarchy moved so that its object record (vt slot 31) is at mat, at an alpha):
// its globs as extra draws
void AppendAloExtraDraws(const SCENE& scene, const ALO* palo, const glm::mat4& mat, float alpha, std::vector<EXTRADRAW>& aextra)
{
    const SCENEGLOBSET* psgs = PsgsOfAlo(scene, palo);
    if (!psgs || !palo->pglobset) return;
    const XF* pxfObj = palo->apxfRef[2];
    const glm::mat4 matAlo = pxfObj ? pxfObj->matWorld : palo->axf.empty() ? glm::mat4(1.0f) : palo->axf[0]->matWorld;
    const glm::mat4 matAloInv = InverseAffineMatrix4(matAlo);  // orig 0x1A5EE8 -> 0x11C318
    const GLOBSET& gs = *palo->pglobset;
    for (size_t i = 0; i < gs.aglob.size(); ++i)
    {
        const GLOB& glob = gs.aglob[i];
        // orig: 0x168638: an external matrix retains the model's group visibility.
        if (!FAloGlobShown(palo, glob.n0, !(glob.grfGroup & 0x800))) continue;
        const XF* pxf = glob.oidOwner >= 0 ? PxfFindAlo(const_cast<ALO*>(palo), glob.oidOwner) : nullptr;
        glm::mat4 mg = pxf ? pxf->matWorld : matAlo;
        if (glob.fMatrix) mg = mg * glob.dmat;
        EXTRADRAW x;
        x.psgs = psgs;
        x.iglob = i;
        x.mat = mat * (matAloInv * mg);
        x.alpha = alpha;
        SetExtraDrawBounds(x, palo, mat * matAloInv);
        aextra.push_back(x);
    }
}

// orig: 0x0015BE28 from WakeSoWater 0x15A2C0 (the spray of an object moving faster than 500 through the water: a
// one-off EMITB (0x159118's defaults D_002DE5B0 / D_002DE5F0) of c kind-1 RIPs of the water's +0x178 model, their
// ripple model (EMITS +0xC) the water's +0x17C and their touch object the water: at pos raised 30, upward within
// 0.39..1.05 rad at 0.5..0.75 d, the object's gravity, life 5, size 8..12, alpha 1 - u^2. They fall back into the
// water, where 0x1D0D28 makes their ripples.
void LaunchWaterSpray(const LO* ploSpray, const LO* ploRipple, LO* ploWater, const glm::vec3& pos, float d, const glm::vec3& accel, int c)
{
    if (!ploSpray) return;
    if (FOtherSw(g_psw)) InitEmittersOfSw(g_psw);
    EMITTERN tmp;
    IMG& b = tmp.b;
    b.Wr<int32_t>(0x00, 1);
    b.Wr<int32_t>(0x04, 1);              // (a RIP emitter)
    b.Wr<int32_t>(0x20, -1);
    b.WrV(0x30, pos + glm::vec3(0.0f, 0.0f, 30.0f));
    b.WrV(0x40, glm::vec3(0.0f, 0.0f, 1.0f));
    // EMITV (+0x50)
    b.Wr<float>(0x50, d * 0.5f);
    b.Wr<float>(0x54, d * 0.75f);
    b.Wr<float>(0x58, 0.0f);
    b.Wr<float>(0x5C, 1.0f);
    b.WrV(0x60, accel);
    b.WrV(0x70, glm::vec3(0.0f, 0.0f, 1.0f));
    b.Wr<float>(0x88, 1.0f);
    b.Wr<float>(0x8C, 5.0f);
    b.Wr<float>(0x90, 0.39269909f);
    b.Wr<float>(0x94, 1.0471976f);
    b.Wr<float>(0xB0, 1.0f);
    b.Wr<float>(0xB8, -1.0f);
    // EMITP (+0xC0)
    b.Wr<int32_t>(0xC0, 1);              // RIP kind 1
    b.Wr<int32_t>(0xC4, 0);              // the shared group
    b.Wr<int32_t>(0xC8, ploSpray->oid);
    b.Wr<int32_t>(0xCC, ploRipple ? ploRipple->oid : 0);
    b.Wr<int32_t>(0xD0, ploWater ? ploWater->oid : 0);
    b.Wr<float>(0xD8, 8.0f);
    b.Wr<float>(0xDC, 12.0f);
    b.Wr<float>(0xE0, 1.0f);
    tmp.s.aplo[2] = const_cast<LO*>(ploSpray);
    tmp.s.aplo[3] = const_cast<LO*>(ploRipple);
    tmp.s.aplo[4] = ploWater;
    if (std::getenv("SLY2N_RIPHIT"))
        std::fprintf(stderr, "RIPHIT spray t %.3f at %.0f %.0f %.0f d %.0f c %d\n", g_clock.t, pos.x, pos.y, pos.z, d, c);
    EmitParticles(&tmp, c);
}

// orig: 0x001CE888(G, 4, 0) + 0x1D0108 (a pickup's glint: the model at an object's point, life dt)
bool FLaunchWaterRipple(const LO* pmodel, const glm::vec3& pos, float gScale)
{
    if (!pmodel) return false;
    if (FOtherSw(g_psw)) InitEmittersOfSw(g_psw);
    RIPG* pg = PripgNew(0);
    RIPN r;
    r.kind = 7;
    r.tBirth = g_clock.t;   // 0x1CE920 (slot 1): the start, life 1, the default alpha, identity, at pos
    r.life = 1.0f;
    r.pos = r.posPrev = pos;
    r.size[0] = gScale;     // +0x20 / +0x24 (set by the caller), +0x28 0
    r.size[1] = gScale * 0.6f;
    r.size[2] = 0.0f;
    r.pmodel = pmodel;      // +0x1C
    pg->arip.push_back(std::move(r));
    return true;
}

bool FLaunchGlintRip(const LO* pmodel, const LO* ploAt, float dt)
{
    if (FOtherSw(g_psw)) InitEmittersOfSw(g_psw);
    RIPG* pg = PripgNew(0);
    RIPN r;
    r.kind = 4;
    r.tBirth = g_clock.t;
    r.pos = r.posPrev = PosLoWorld(ploAt);  // (slot 1 at 0x1A62E8 of the object)
    r.life = dt;
    r.alpha[0] = 0.0f; r.alpha[1] = 4.0f; r.alpha[2] = -4.0f;  // D_002C1F90
    r.size[0] = 0.25f; r.size[1] = 3.0f; r.size[2] = -3.0f;    // D_002C1FA0
    r.pmodel = pmodel;
    r.ploFollow = ploAt;
    pg->arip.push_back(std::move(r));
    return true;
}
