// pe.cpp: Sly 3's particle effects (class 0x2D; pe.h). Re-derived from the Sly 3 executable (orig3 addresses); the
// particle systems themselves are emitter.cpp's (Sly 2's), which the PE streams share in the original too.
#include "pe.h"

#include "activate.h"
#include "alo.h"
#include "animfind.h"
#include "worldaccess.h"
#include "bis.h"
#include "brx.h"
#include "clock.h"
#include "emitter.h"
#include "game/cond3.h"
#include "game/sly3.h"
#include "readtrace.h"
#include "sound.h"
#include "sw.h"
#include "update.h"
#include "xfmath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

PESTATS g_pestats;

void LoadLoxFromBrxPub(LO* plo, CBinaryInputStream* pbis);                          // loaders.cpp
void LoadEmitmeshFromBrxPub(LOXEMIT::EMITMESH* pem, CBinaryInputStream* pbis);       // loaders.cpp

namespace
{
constexpr float PI = 3.14159265f;
constexpr int CID_PE = 0x2D;

// ------------------------------------------------------------ the loader
// orig3: 0x1FE270 (PE 0x2D): LoadLoxFromBrx; then, with the transform base +0x40 pushed, S16 c items, each with an
// object header (S8 mode; mode 1: U8 filters, U8 conditions, S16 filters, S32 conditions; S16 cid, S32 oid, S16 n,
// S32 cb): an item that passes its filters / conditions and whose cid is 0xBF (0x1FD758) or 0xC0 (0x1FDE70) is made and
// loaded through its interface (vt +0x20 -> 0x1FD988 / 0x1FDEE8, slot 7: 0x1FD9D8 -> 0x1FD198: the options 0x1519C8,
// then 0x1FCF10: the emission mesh (0x1775B8) when the position kind (EOPID 558) is 3); the others are skipped by cb.
void LoadPeFromBrx(LO* plo, CBinaryInputStream* pbis)
{
    LOADFN(0x1FE270);
    LOPE* ppe = static_cast<LOPE*>(plo);
    LoadLoxFromBrxPub(ppe, pbis);
    PushXfBase(ppe->pxf);
    ++g_pestats.cpe;
    const int c = pbis->S16Read();
    for (int i = 0; i < c; ++i)
    {
        const int mode = pbis->S8Read();
        std::vector<int16_t> afilter;
        std::vector<int32_t> acond;
        if (mode == 1)
        {
            afilter.resize(pbis->U8Read());
            acond.resize(pbis->U8Read());
            for (int16_t& f : afilter) f = pbis->S16Read();
            for (int32_t& k : acond) k = pbis->S32Read();
        }
        PECOMP comp;
        comp.cid = pbis->S16Read();
        comp.oid = pbis->S32Read();
        comp.n = pbis->S16Read();
        const int32_t cb = pbis->S32Read();
        const bool fPass = mode != 1 || (FFilterMatches3(int(afilter.size()), afilter.data()) &&
                                         FConditionsMatch3(int(acond.size()), acond.data()));
        if (!fPass || (comp.cid != 0xBF && comp.cid != 0xC0))
        {
            pbis->Skip(std::size_t(cb));
            continue;
        }
        const std::size_t ibStart = pbis->GetOffset();
        LoadOptionsFromBrx(comp.aopt, nullptr, pbis);
        int kPos = 0;
        for (const OPT& o : comp.aopt)
            if (o.eopid == 558 && !o.aval.empty()) kPos = o.aval[0].n;
        if (kPos == 3)
        {
            comp.pmesh = std::make_shared<LOXEMIT::EMITMESH>();
            LoadEmitmeshFromBrxPub(comp.pmesh.get(), pbis);
        }
        if (pbis->GetOffset() != ibStart + std::size_t(cb))
        {
            ++g_pestats.cbad;
            if (std::getenv("SLY3_PEDBG"))
                std::fprintf(stderr, "pe %d component 0x%X %d: read %zu of %d\n", ppe->oid, comp.cid, comp.oid,
                             pbis->GetOffset() - ibStart, cb);
        }
        ++g_pestats.ccomp;
        ppe->acomp.push_back(std::move(comp));
    }
    PopXfBase();
}

// ------------------------------------------------------------ the records (from a component's options)
// orig3: the getters 0x1FD540 (position, comp +0x10: 0x60 bytes), 0x1FD6A0 (velocity / life, +0x14: 0x90), 0x1FDC18
// (shared, +0x1C: 0x30; its +0x20 the render record 0x220), defaults 0x1FCCE8 / the template 0x347EF0
struct PEPOS
{
    int k = 0;                       // +0x48 (558): 0 a point, 1 a box, 2 a curve object, 3 the mesh
    glm::vec3 v0{0.0f}, v1{0.0f};    // +0x00 (559 the point / 560 the box's low), +0x10 (561 its high)
    int32_t oid = -1;                // +0x00 (562: kind 2's object)
    int kNormal = 0;                 // +0x4C (563)
    glm::vec3 dir{1, 0, 0};          // +0x30 (564; q[0x317530])
    float offLo = 0, offHi = 0;      // +0x40 (565): an offset along the normal
};
struct PEVEL
{
    glm::vec3 accel{0, 0, -1500};    // +0x00 (572)
    glm::vec3 axisSpin{0, 0, 1};     // +0x10 (574)
    float clqScale[3] = {25, 0, 0};  // +0x20 (581)
    float clqAlpha[3] = {1, 0, -1};  // +0x30 (580)
    float speedLo = 750, speedHi = 750;  // +0x40 (569)
    float tiltLo = 0, tiltHi = 0;    // +0x48 (575)
    float advLo = 0, advHi = 0;      // +0x50 (578): a head start along the velocity
    float ageLo = 0, ageHi = 0;      // +0x58 (579): an age at birth
    int kFrameVel = 0;               // +0x60 (567): 0 the frame's motion added, 1 turned by the frame, 2 as is
    int k64 = 0;                     // +0x64 (568)
    float gDrag = -1;                // +0x68 (571)
    float rSvz = 1;                  // +0x6C (570)
    float gSpin = 0;                 // +0x70 (573)
    float uJitter = 1;               // +0x74 (576)
    float life = 1;                  // +0x78 (577)
    bool fPrewarm = false;           // +0x7C (566)
};
struct PECMD   // a command (0x1C bytes at comp +0xC; templates 0x3255A8..): 0 burst, 1 rate, 2 sound, 3 stop, 4 wait
{
    int k = 0;
    float lo = 0, hi = 0, dur = -1;
    // (kind 2, 0x1FD390: +4 the sound id, +8 / +0xC the falloff, +0x10 the volume, +0x14 the pitch)
    int sfx = -1;
    float sFalloff0 = 0, sFalloff1 = 0, gVolume = 1, gPitch = 0;
};
struct PEREC
{
    PEPOS pos;
    PEVEL vel;
    PEPSYSDESC desc;
    int32_t oidFrame = -1;           // shared +0x10 (589): the particles' frame object
    std::vector<PECMD> acmd;
    int cLoop = 0;                   // comp +8
    // 0xC0 (a RIP stream; its record 0x1FE0C0 / 0x1FDFD0): the RIPs' kind (594's setter, record +0x1C), the model
    // (596, +0xC: stream +0x5C), a second object (597, +0x10: stream +0x60), the size range (598, +0x14)
    int kRip = 3;
    int32_t oidModel = -1, oid2 = -1;
    float sizeLo = 1, sizeHi = 1;
};

float G(const OPTVAL& v, int i = 0) { return v.ag[size_t(i)]; }

// a method option's arguments (RefSetArgsFromSplice): numbers in order, ranges as two
std::vector<float> ArgsOf(const OPT& o)
{
    // (by the option kinds of RefReadOption: 1 float, 2 / 9 three floats, 10 two, else an integer)
    auto add = [](std::vector<float>& a, const OPTVAL& v) {
        const int k = v.k & ~0x8000;
        if (k == 1) a.push_back(v.ag[0]);
        else if (k == 2 || k == 9) for (int i = 0; i < 3; ++i) a.push_back(v.ag[size_t(i)]);
        else if (k == 10) { a.push_back(v.ag[0]); a.push_back(v.ag[1]); }
        else a.push_back(float(v.n));
    };
    std::vector<float> a;
    for (const OPTVAL& v : o.aval)
    {
        if (v.k & 0x8000) for (const OPTVAL& w : v.alist) add(a, w);
        else add(a, v);
    }
    return a;
}

PEREC RecOfComp(const PECOMP& comp)
{
    PEREC r;
    r.cLoop = comp.n;   // (the item header's S16; option 557 sets it)
    PEPSYSDESC& d = r.desc;
    for (const OPT& o : comp.aopt)
    {
        if (o.aval.empty() && o.eopid != 555) continue;
        const OPTVAL v = o.aval.empty() ? OPTVAL{} : o.aval[0];
        switch (o.eopid)
        {
        case 558: r.pos.k = v.n; break;
        case 559: r.pos.v0 = glm::vec3(G(v), G(v, 1), G(v, 2)); break;
        case 560: r.pos.v0 = glm::vec3(G(v), G(v, 1), G(v, 2)); break;
        case 561: r.pos.v1 = glm::vec3(G(v), G(v, 1), G(v, 2)); break;
        case 562: r.pos.oid = v.n; break;
        case 563: r.pos.kNormal = v.n; break;
        case 564: r.pos.dir = glm::vec3(G(v), G(v, 1), G(v, 2)); break;
        case 565: r.pos.offLo = G(v); r.pos.offHi = G(v, 1); break;
        case 566: r.vel.fPrewarm = v.n != 0; break;
        case 567: r.vel.kFrameVel = v.n; break;
        case 568: r.vel.k64 = v.n; break;
        case 569: r.vel.speedLo = G(v); r.vel.speedHi = G(v, 1); break;
        case 570: r.vel.rSvz = G(v); break;
        case 571: r.vel.gDrag = G(v); break;
        case 572: r.vel.accel = glm::vec3(G(v), G(v, 1), G(v, 2)); break;
        case 573: r.vel.gSpin = G(v); break;
        case 574: r.vel.axisSpin = glm::vec3(G(v), G(v, 1), G(v, 2)); break;
        case 575: r.vel.tiltLo = G(v); r.vel.tiltHi = G(v, 1); break;
        case 576: r.vel.uJitter = G(v); break;
        case 577: r.vel.life = G(v); break;
        case 578: r.vel.advLo = G(v); r.vel.advHi = G(v, 1); break;
        case 579: r.vel.ageLo = G(v); r.vel.ageHi = G(v, 1); break;
        case 580: for (int i = 0; i < 3; ++i) r.vel.clqAlpha[i] = G(v, i); break;
        case 581: for (int i = 0; i < 3; ++i) r.vel.clqScale[i] = G(v, i); break;
        case 582: d.kDraw = v.n; break;              // the setter 0x1FDB80: shared +0x24
        case 583: d.kUpdate = v.n; break;            // shared +0x28
        case 584: d.shd = v.n; break;                // render +0
        case 585: d.fRandTex = v.n != 0; break;      // shared +0x14
        case 586: d.dTex = G(v); break;              // shared +0x1C
        case 588: d.fAlphaOne = v.n != 0; break;     // shared +0x18
        case 589: r.oidFrame = v.n; break;           // shared +0x10
        case 594: r.kRip = v.n; break;               // (0xC0) 0x1FDFD0: the RIPs' kind
        case 596: r.oidModel = v.n; break;
        case 597: r.oid2 = v.n; break;
        case 598: r.sizeLo = G(v); r.sizeHi = G(v, 1); break;
        case 590: d.fSpinFixed = v.n != 0; break;    // (kind 2) shared +0
        case 591: d.fRoll = v.n != 0; break;         // shared +4
        case 592: d.spinLo = G(v); d.spinHi = G(v, 1); break;   // shared +8
        case 593: d.gFlying = G(v); break;           // (kind 1) shared +0
        case 587:   // 0x1FDD40: a colour (r, g, b, a), or a pair of them (render +0x214 then set by the first call)
        {
            const std::vector<float> a = ArgsOf(o);
            auto rgba = [&](size_t i) {
                uint32_t u = 0;
                for (size_t j = 0; j < 4; ++j) u |= uint32_t(uint8_t(std::clamp(i + j < a.size() ? a[i + j] : 255.0f, 0.0f, 255.0f))) << (8 * j);
                return u;
            };
            if (d.argba.empty()) d.fColorPairs = a.size() >= 8;
            if (d.argba.size() < 0x20) d.argba.push_back(rgba(0));
            if (d.fColorPairs && d.argba.size() < 0x20) d.argba.push_back(rgba(4));
            break;
        }
        case 557: r.cLoop = int(ArgsOf(o).empty() ? 0 : ArgsOf(o)[0]); break;   // 0x1FD4D0: comp +8, the repeat count
        case 552: case 553: case 554: case 555: case 556:   // the commands (0x1FD2C0 / 0x1FD320 / 0x1FD390 / 0x1FD400 / 0x1FD460)
        {
            if (r.acmd.size() >= 0x20) break;
            const std::vector<float> a = ArgsOf(o);
            PECMD cmd;
            cmd.k = o.eopid == 553 ? 1 : o.eopid == 554 ? 2 : o.eopid == 555 ? 3 : o.eopid == 556 ? 4 : 0;
            if (cmd.k == 1) { cmd.lo = a.size() > 0 ? a[0] : 0; cmd.hi = a.size() > 1 ? a[1] : cmd.lo; cmd.dur = a.size() > 2 ? a[2] : -1; }
            else if (cmd.k == 4 || cmd.k == 0) { cmd.lo = a.size() > 0 ? a[0] : 0; cmd.hi = a.size() > 1 ? a[1] : cmd.lo; cmd.dur = 0; }
            else if (cmd.k == 2)
            {
                cmd.dur = 0;
                cmd.sfx = a.size() > 0 ? int(a[0]) : -1;
                cmd.sFalloff0 = a.size() > 1 ? a[1] : 0;
                cmd.sFalloff1 = a.size() > 2 ? a[2] : 0;
                cmd.gVolume = a.size() > 3 ? a[3] : 1;
                cmd.gPitch = a.size() > 4 ? a[4] : 0;
            }
            else cmd.dur = 0;
            if (cmd.k == 0) { cmd.lo = std::trunc(cmd.lo); cmd.hi = std::trunc(cmd.hi); }   // (0x1FD2C0: cvt.w.s)
            r.acmd.push_back(cmd);
            break;
        }
        default: break;
        }
    }
    d.life = r.vel.life;
    for (int i = 0; i < 3; ++i) { d.clqAlpha[i] = r.vel.clqAlpha[i]; d.clqScale[i] = r.vel.clqScale[i]; }
    d.accel = r.vel.accel;
    d.axisSpin = r.vel.axisSpin;
    d.gSpin = r.vel.gSpin;
    d.gDrag = r.vel.gDrag;
    // 0x1394C8's rule kept by 0x200F50 (kind 0 without forces moves as kind 1)
    if (d.kUpdate == 0 && glm::length(d.accel) < 0.0001f && d.gDrag == 0.0f && d.gSpin == 0.0f) d.kUpdate = 1;
    return r;
}

// ------------------------------------------------------------ the camera (the drawn one)
glm::vec3 s_posCam{0.0f};
glm::mat3 s_matCamRows{1.0f};
glm::mat4 s_matClip{1.0f};
bool s_fCam = false;

// orig3: 0x18E728 (a sphere against the camera's frustum; the PE's +0x34 radius)
bool FSphereSeen(const glm::vec3& c, float r)
{
    if (!s_fCam) return true;
    const glm::mat4 mt = glm::transpose(s_matClip);
    const glm::vec4 aplane[5] = {mt[3] + mt[0], mt[3] - mt[0], mt[3] + mt[1], mt[3] - mt[1], mt[3] - mt[2]};
    for (glm::vec4 p : aplane)
    {
        p /= glm::length(glm::vec3(p));
        if (glm::dot(glm::vec3(p), c) + p.w < -r) return false;
    }
    return true;
}

glm::mat4 MatCamera()
{
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(s_matCamRows[0], 0.0f);
    m[1] = glm::vec4(s_matCamRows[1], 0.0f);
    m[2] = glm::vec4(s_matCamRows[2], 0.0f);
    m[3] = glm::vec4(s_posCam, 1.0f);
    return m;
}

glm::mat4 MatWorldOfPe(const LO* plo)
{
    const LOX* px = static_cast<const LOX*>(plo);
    if (px->pxf) return px->pxf->matWorld * px->mat;
    if (plo->paloParent) return MatWorldOfAlo(plo->paloParent) * px->mat;
    return px->mat;
}

int NOpt(const LO* plo, int eopid, int nDefault)
{
    for (const OPT& o : plo->aopt)
        if (o.eopid == eopid && !o.aval.empty()) return o.aval[0].n;
    return nDefault;
}

// ------------------------------------------------------------ the streams and instances
struct STREAM   // orig3: 0x200868 (a 0xBF component's stream; its runner 0x1FEEF8)
{
    const PECOMP* pcomp = nullptr;
    PEREC rec;
    PEPSYS* ppsys = nullptr;
    bool fRip = false;               // a 0xC0 stream (0x202220): its particles are RIPs
    const LO* pmodel = nullptr;      // +0x5C
    const LO* plo2 = nullptr;        // +0x60
    int icmd = -1;                   // +0x14
    float tCmd = 0;                  // +0x10
    float gAcc = 0;                  // +0x18 (kind 4: the wait)
    float gRate = 0;                 // +0x1C
    int cLoop = 0;                   // +0x20
    bool fDone = false;
    CSndRef snd;                     // +0xC (a looping sound of command 2, stopped by the next one / the end)
};
struct INST     // orig3: 0x202A88 (vt 0x361538)
{
    LO* ppe = nullptr;               // +0x10 (kind 2: the prototype)
    int kind = 0;
    float gScale = 1.0f;             // +0x68
    std::vector<STREAM> astream;
    bool fSeen = false;
};
std::map<const LO*, INST> s_mpinst;
// kind 0 (one-shot) instances (0x1FE840: keyed by the frame object +0x14 and the particles' object +0x60)
struct ONESHOT
{
    INST inst;
    const LO* ploFrame = nullptr;
    glm::mat4 matFrame{1.0f};
};
std::vector<ONESHOT> s_aoneshot;
uint64_t s_gen = 0;
const SW* s_psw = nullptr;
uint32_t s_serial = 0;

// orig3: 0x1FF3E0 (the next command: its start time; a rate's random rate, a wait's random time; at the end the
// list again while the repeat count +0x20 allows)
void NextCommand(STREAM& s)
{
    ++s.icmd;
    s.tCmd = g_clock.t;
    if (s.icmd >= int(s.rec.acmd.size()))
    {
        if (s.cLoop == 0) { s.fDone = true; return; }
        if (s.cLoop > 0 && --s.cLoop <= 0) { s.fDone = true; return; }
        s.icmd = 0;
    }
    if (s.icmd < int(s.rec.acmd.size()))
    {
        const PECMD& c = s.rec.acmd[size_t(s.icmd)];
        if (c.k == 1) { s.gAcc = 0; s.gRate = GRandInRange(c.lo, c.hi); }
        else if (c.k == 4) s.gAcc = GRandInRange(c.lo, c.hi);
    }
}

// orig3: 0x1FF6B0 (the positions, and the normals of kinds 2 / 3) + 0x1FFC20 (the velocities) + 0x2001C0 (the times)
// + 0x200328 (into the world: the emission frame mat; the velocity per +0x60), then 0x200C10
void EmitParticles(STREAM& s, int c, const glm::mat4& matFrame, const glm::vec3& velFrame, float advLo, float advHi)
{
    if (c <= 0 || (!s.ppsys && !s.fRip)) return;
    c = std::min(c, 2000);
    const PEPOS& rp = s.rec.pos;
    const PEVEL& rv = s.rec.vel;
    const size_t cp = static_cast<size_t>(c);
    std::vector<glm::vec3> apos(cp), anormal(cp, glm::vec3(1, 0, 0)), avel(cp);
    std::vector<float> at0(cp), at1(cp);
    // positions
    for (int i = 0; i < c; ++i)
    {
        glm::vec3& p = apos[size_t(i)];
        switch (rp.k)
        {
        case 1: p = glm::vec3(GRandInRange(rp.v0.x, rp.v1.x), GRandInRange(rp.v0.y, rp.v1.y), GRandInRange(rp.v0.z, rp.v1.z)); break;
        case 3:   // area-weighted triangle, a random point of it
            if (const LOXEMIT::EMITMESH* pm = s.pcomp->pmesh.get(); pm && !pm->atri.empty())
            {
                float sTotal = 0;
                for (const auto& t : pm->atri) sTotal += t.sArea;
                float g = GRandInRange(0.0f, 1.0f) * sTotal;
                size_t it = 0;
                for (; it + 1 < pm->atri.size(); ++it)
                    if ((g -= pm->atri[it].sArea) <= 0.0f) break;
                const auto& t = pm->atri[it];
                float u = GRandInRange(0.0f, 1.0f), v = GRandInRange(0.0f, 1.0f);
                if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
                auto P = [&](int k) { return size_t(t.aipos[k]) < pm->apos.size() ? pm->apos[size_t(t.aipos[k])] : glm::vec3(0.0f); };
                p = P(0) * (1.0f - u - v) + P(1) * u + P(2) * v;
                anormal[size_t(i)] = glm::normalize(glm::cross(P(1) - P(0), P(2) - P(0)) + glm::vec3(0, 0, 1e-6f));
            }
            break;
        default: p = rp.v0; break;   // 0 (2, a curve object: TODO(effects) EvalCrvS 0x22E4D0; its point)
        }
    }
    // the normals (0x1FFC20's table 0x325690)
    for (int i = 0; i < c; ++i)
    {
        glm::vec3& n = anormal[size_t(i)];
        switch (rp.kNormal)
        {
        case 0: n = glm::length(rp.dir) > 1e-4f ? glm::normalize(rp.dir) : glm::vec3(1, 0, 0); break;
        case 1: { const glm::vec3 d = apos[size_t(i)] - rp.dir; n = glm::length(d) > 1e-4f ? glm::normalize(d) : glm::vec3(1, 0, 0); break; }
        case 4: { const glm::vec3 d = glm::inverse(glm::mat3(matFrame)) * (s_matCamRows * rp.dir); n = glm::length(d) > 1e-4f ? glm::normalize(d) : glm::vec3(1, 0, 0); break; }
        case 5: if (rp.k != 3) n = glm::vec3(1, 0, 0); break;
        default: if (rp.k != 2) n = glm::vec3(1, 0, 0); break;
        }
        if (rp.offLo != 0.0f || rp.offHi != 0.0f) apos[size_t(i)] += n * GRandInRange(rp.offLo, rp.offHi);
    }
    // the velocities: one ring of c around the normal, the tilt range's middle (+ pi / 2), jittered by +0x74
    {
        const float dPan = 2.0f * PI / float(c), dTilt = rv.tiltHi - rv.tiltLo;
        float pan = GRandInRange(0.0f, 2.0f * PI);
        const float tilt0 = rv.tiltLo + 0.5f * dTilt + PI * 0.5f;
        for (int i = 0; i < c; ++i)
        {
            float radPan = pan, radTilt = tilt0;
            if (rv.uJitter != 0.0f)
            {
                radPan = pan + rv.uJitter * GRandInRange(-0.5f, 0.5f) * dPan;
                radTilt = tilt0 + rv.uJitter * GRandInRange(-0.5f, 0.5f) * dTilt;
            }
            radPan = RadNormalize(radPan);
            radTilt = RadNormalize(radTilt);
            pan += dPan;
            const float sp = GRandInRange(rv.speedLo, rv.speedHi);
            glm::vec3 v(std::cos(radTilt) * std::cos(radPan) * sp, std::cos(radTilt) * std::sin(radPan) * sp, std::sin(radTilt) * sp);
            v.z *= rv.rSvz;
            const glm::vec3& n = anormal[size_t(i)];
            glm::vec3 across = glm::cross(glm::vec3(1, 0, 0), n);
            if (glm::dot(across, across) < 1e-4f) across = glm::cross(glm::vec3(0, 1, 0), n);
            const glm::vec3 a0 = glm::normalize(across), a1 = glm::normalize(glm::cross(n, a0));
            avel[size_t(i)] = a0 * v.x + a1 * v.y + n * v.z;
        }
    }
    // the times; a head start along the velocity
    for (int i = 0; i < c; ++i)
    {
        at0[size_t(i)] = g_clock.t - GRandInRange(rv.ageLo, rv.ageHi);
        at1[size_t(i)] = at0[size_t(i)] + rv.life;
    }
    if (advLo != 0.0f || advHi != 0.0f)
        for (int i = 0; i < c; ++i)
        {
            const float g = GRandInRange(advLo, advHi);
            apos[size_t(i)] += avel[size_t(i)] * g;
            at0[size_t(i)] -= g;
            at1[size_t(i)] -= g;
        }
    // into the world (0x200328): the frame's matrix; the velocity turned by it (kinds 0 / 1) plus its motion (0)
    const glm::mat3 r3(matFrame);
    for (int i = 0; i < c; ++i)
    {
        apos[size_t(i)] = glm::vec3(matFrame * glm::vec4(apos[size_t(i)], 1.0f));
        if (rv.kFrameVel != 2) avel[size_t(i)] = r3 * avel[size_t(i)];
        if (rv.kFrameVel == 0) avel[size_t(i)] += velFrame;
    }
    if (!s.fRip)
    {
        EmitPepsys(s.ppsys, c, apos.data(), avel.data(), at0.data(), at1.data());
        return;
    }
    // orig3: 0x202330: one RIP each (its init with a random size, the velocity record's size / alpha curves, motion,
    // the frame's rotation; then the kind's slot 2 with the record and the second object)
    if (!s.pmodel) return;
    for (int i = 0; i < c; ++i)
    {
        PERIPDESC d;
        d.kind = s.rec.kRip;
        d.pmodel = s.pmodel;
        d.plo2 = s.plo2;
        d.pos = apos[size_t(i)];
        d.vel = avel[size_t(i)];
        d.tBirth = at0[size_t(i)];
        d.life = at1[size_t(i)] - at0[size_t(i)];
        d.size[0] = GRandInRange(s.rec.sizeLo, s.rec.sizeHi);
        if (rv.clqScale[1] != 0.0f || rv.clqScale[2] != 0.0f) { d.size[1] = rv.clqScale[1]; d.size[2] = rv.clqScale[2]; }
        else d.size[1] = d.size[2] = 0.0f;
        for (int k = 0; k < 3; ++k) d.alpha[k] = rv.clqAlpha[k];
        d.accel = rv.accel;
        d.axisSpin = rv.axisSpin;
        d.gSpin = rv.gSpin;
        d.gDrag = rv.gDrag;
        d.ori = r3;
        LaunchPeRip(d);
    }
}

// orig3: 0x1FF100 (the runner: the current command; returns true when the list is over)
bool FRunCommands(STREAM& s, float gScale, const glm::mat4& matFrame, const glm::vec3& velFrame, const glm::mat4* pmatSound = nullptr)
{
    for (int guard = 0; guard < 64 && !s.fDone && s.icmd < int(s.rec.acmd.size()); ++guard)
    {
        const PECMD& c = s.rec.acmd[size_t(s.icmd)];
        const float tIn = g_clock.t - s.tCmd;
        bool fNext = false;
        switch (c.k)
        {
        case 0:   // a burst: rand(lo, hi) x the instance's scale at once
            EmitParticles(s, int(GRandInRange(gScale * c.lo, gScale * c.hi)), matFrame, velFrame, s.rec.vel.advLo, s.rec.vel.advHi);
            fNext = c.dur >= 0.0f && c.dur <= tIn;
            break;
        case 1:   // a rate: scale x rate x dt, the fraction kept
        {
            s.gAcc += gScale * s.gRate * g_clock.dt;
            const int n = int(s.gAcc);
            s.gAcc -= float(n);
            EmitParticles(s, n, matFrame, velFrame, s.rec.vel.advLo, s.rec.vel.advHi);
            fNext = c.dur >= 0.0f && c.dur <= tIn;
            break;
        }
        case 2:   // a sound (0x1FF240: PsndNewPlain 0x2459F8, falloff / volume / pitch, at the frame; a looping one kept)
        {
            fNext = true;
            if (s.snd) { s.snd->Stop(0.0f); s.snd = CSndRef(); }
            if (c.sfx < 0) break;
            // (a kind-2 effect's frame is its prototype's space: natively at its first clone drawn, as its RIPs)
            const glm::vec3 pos(pmatSound ? (*pmatSound)[3] : matFrame[3]);
            CSnd* psnd = PsndStartSound(c.sfx, 1.0f, 0, nullptr, &pos);
            if (psnd)
            {
                psnd->SetFalloff(c.sFalloff0, c.sFalloff1);
                psnd->SetVolume(c.gVolume);
                psnd->SetPitch(c.gPitch);
                if (psnd->Kind() == 2) s.snd = CSndRef(psnd);
            }
            if (static const bool s_f = std::getenv("SLY3_PESND") != nullptr; s_f)   // (debug: the effects' sounds)
                std::printf("pesnd sfx %d at %.0f %.0f %.0f falloff %g %g volume %g pitch %g %s\n", c.sfx, pos.x, pos.y, pos.z, c.sFalloff0,
                            c.sFalloff1, c.gVolume, c.gPitch, !psnd ? "(no sound)" : psnd->Kind() == 2 ? "(looping)" : "");
            break;
        }
        case 3: s.fDone = true; break;   // the end
        case 4: fNext = s.gAcc <= tIn; break;
        default: fNext = true; break;
        }
        if (!fNext) break;
        NextCommand(s);
    }
    return s.fDone;
}

void ResetPes()
{
    s_mpinst.clear();
    s_aoneshot.clear();
}

LO* s_ploNewFor = nullptr;   // (the effect object whose streams StreamNew makes: the oids are found near it)

STREAM StreamNew(const PECOMP& comp)
{
    STREAM s;
    s.pcomp = &comp;
    s.rec = RecOfComp(comp);
    s.cLoop = s.rec.cLoop;
    if (comp.cid == 0xC0)
    {
        s.fRip = true;
        LO* ploNear = s_ploNewFor;
        s.pmodel = s.rec.oidModel != -1 && ploNear ? PloFindSwNearest(ploNear, s.rec.oidModel) : nullptr;
        s.plo2 = s.rec.oid2 != -1 && ploNear ? PloFindSwNearest(ploNear, s.rec.oid2) : nullptr;
    }
    else s.ppsys = PpepsysNew(s.rec.desc);
    NextCommand(s);
    return s;
}

// orig3: 0x1FF4D8 (a new stream whose first command is a rate and whose velocity record has +0x7C: rate x scale x
// life particles at once, their head start over the whole life)
void PrewarmStream(STREAM& s, float gScale, const glm::mat4& matFrame, const glm::vec3& velFrame)
{
    if (!s.rec.vel.fPrewarm || s.icmd < 0 || s.icmd >= int(s.rec.acmd.size()) || s.rec.acmd[size_t(s.icmd)].k != 1) return;
    EmitParticles(s, int(gScale * s.gRate * s.rec.vel.life), matFrame, velFrame, 0.0f, s.rec.vel.life);
}

const LO* PloProto(const LO* plo)
{
    while (plo->ploCloneOf) plo = plo->ploCloneOf;
    return plo;
}

const std::vector<PECOMP>* PacompOf(const LO* plo)
{
    for (const LO* p = plo; p; p = p->ploCloneOf)
        if (const LOPE* ppe = dynamic_cast<const LOPE*>(p); ppe && !ppe->acomp.empty()) return &ppe->acomp;
    return nullptr;
}
}  // namespace

void SetPeCamera(const glm::vec3& pos, const glm::mat3& matRows, const glm::mat4& matWorldToClip)
{
    s_posCam = pos;
    s_matCamRows = matRows;
    s_matClip = matWorldToClip;
    s_fCam = true;
}

// orig3: 0x202D80 (each instance: 0x203170's test, then 0x1FEF60 on its streams: the runner when it may emit, the
// move 0x200F50) and 0x202E58's matrices for the draw
void UpdatePes(SW* psw)
{
    if (!psw) return;
    if (s_gen != GenPeParticles() || s_psw != psw || s_serial != psw->serial)
    {
        ResetPes();
        s_gen = GenPeParticles();
        s_psw = psw;
        s_serial = psw->serial;
    }
    static const bool s_fOff = std::getenv("SLY3_NO_PE") != nullptr;
    if (s_fOff) return;
    static const bool s_fDbg = std::getenv("SLY3_PEDBG") != nullptr;
    static int s_cUpdate = 0;
    ++s_cUpdate;
    // the instances (OnAdd 0x1FE6B0 / OnRemove 0x1FE670): kinds 1 / 3 their own, kind 2 one per prototype with the
    // clones in the world as its draw matrices (0x202E58: at most 0x20); kind 0 (one-shot, made by the scripts) none yet
    std::map<const LO*, std::vector<const LO*>> mpprotoaclone;
    std::map<const LO*, const LO*> mpinstlo;
    // (kind 2's OnAdd 0x1FE6B0 appends the clone to its definition's list (+0xF4, AppendDlEntry), OnRemove 0x1FE670
    // unlinks it: the list is in the order the clones went into the world; natively the order they are first seen in
    // the world by this update, those of one update in the world's order)
    static std::map<const LO*, uint64_t> s_mpcloneseq;
    static uint64_t s_seqClone = 0;
    static const SW* s_pswClone = nullptr;
    if (s_pswClone != psw) { s_mpcloneseq.clear(); s_pswClone = psw; }
    for (LO* plo : psw->mpcidaplo[CID_PE])
    {
        if (!plo->fInWorld)
        {
            s_mpcloneseq.erase(plo);
            continue;
        }
        const int kind = NOpt(plo, 539, 0);
        if (kind == 2)
        {
            mpprotoaclone[PloProto(plo)].push_back(plo);
            s_mpcloneseq.try_emplace(plo, ++s_seqClone);
        }
        else if (kind == 1 || kind == 3) mpinstlo[plo] = plo;
    }
    for (auto& [pproto, aclone] : mpprotoaclone) mpinstlo[pproto] = pproto;
    for (auto it = s_mpinst.begin(); it != s_mpinst.end();)
    {
        if (!mpinstlo.count(it->first))
        {
            for (STREAM& st : it->second.astream)
            {
                SetPepsysDraw(st.ppsys, true, {});
                if (st.snd) st.snd->Stop(0.0f);   // (its looping sound ends with it)
            }
            it = s_mpinst.erase(it);
        }
        else ++it;
    }
    g_pestats.cinst = 0;
    g_pestats.cstream = 0;
    g_pestats.cparticle = 0;
    // debug: SLY3_PEFIRE=frame:oid plays the kind-0 effects of that oid at that update (at the camera's view point
    // 600 ahead when the effect has no place of its own)
    if (static const char* s_sz = std::getenv("SLY3_PEFIRE"); s_sz)
    {
        int frameFire = -1, oidFire = -1;
        if (std::sscanf(s_sz, "%d:%d", &frameFire, &oidFire) == 2 && s_cUpdate == frameFire)
            for (LO* plo : psw->mpcidaplo[CID_PE])
                if (plo->oid == oidFire && !plo->ploCloneOf) { TriggerPe(plo, nullptr); break; }
    }
    for (auto& [pkey, plo] : mpinstlo)
    {
        LO* ppe = const_cast<LO*>(plo);
        const int kind = NOpt(ppe, 539, 0);
        // the emission frame (+0x14: +0xE0, option 541, default the object; kind 3: the camera) and its motion
        glm::mat4 matFrame(1.0f);
        if (kind == 3) matFrame = MatCamera();
        else if (kind == 1)
        {
            const int32_t oid = NOpt(ppe, 541, -1);
            const LO* pfr = oid != -1 ? PloFindSwNearest(ppe, oid) : nullptr;
            matFrame = pfr && dynamic_cast<const LOX*>(pfr) ? MatWorldOfPe(pfr) : pfr ? glm::translate(glm::mat4(1.0f), PosLoWorld(pfr)) : MatWorldOfPe(ppe);
        }
        // (kind 2: in the prototype's frame, the particles drawn at each clone; natively the clone's own frame is
        // the prototype's local space)
        const std::vector<PECOMP>* pacomp = PacompOf(ppe);
        if (!pacomp) continue;
        auto [itInst, fNew] = s_mpinst.try_emplace(pkey);
        INST& inst = itInst->second;
        if (fNew)
        {
            inst.ppe = ppe;
            inst.kind = kind;
            s_ploNewFor = ppe;
            for (const PECOMP& comp : *pacomp)
                if (comp.cid == 0xBF || comp.cid == 0xC0)
                {
                    inst.astream.push_back(StreamNew(comp));
                    PrewarmStream(inst.astream.back(), inst.gScale, matFrame, glm::vec3(0.0f));
                }
        }
        // 0x203170: may it emit? kind 1: its object seen (its +0x34 radius); kind 2: any clone seen; kind 3: always
        std::vector<glm::mat4> amatDraw;
        bool fEmit = true;
        if (kind == 2)
        {
            std::vector<const LO*> aclone = mpprotoaclone[pkey];
            // (0x202E58: the list's first 0x20 clones drawn)
            std::stable_sort(aclone.begin(), aclone.end(), [](const LO* a, const LO* b) { return s_mpcloneseq[a] < s_mpcloneseq[b]; });
            if (aclone.size() > 0x20) aclone.resize(0x20);
            fEmit = false;
            for (const LO* pc : aclone)
            {
                amatDraw.push_back(MatWorldOfPe(pc));
                fEmit = fEmit || FSphereSeen(PosLoWorld(pc), std::min(pc->sRadius, 1e6f));
            }
        }
        else if (kind == 1) fEmit = FSphereSeen(PosLoWorld(ppe), std::min(ppe->sRadius, 1e6f));
        const glm::mat4 matEmit = kind == 2 ? glm::mat4(1.0f) : matFrame;
        for (STREAM& st : inst.astream)
        {
            // (a kind-2 effect's RIPs are world objects: thrown at the nearest clone)
            if (fEmit) FRunCommands(st, inst.gScale, st.fRip && kind == 2 && !amatDraw.empty() ? amatDraw[0] : matEmit, glm::vec3(0.0f),
                                    kind == 2 && !amatDraw.empty() ? &amatDraw[0] : nullptr);
            // kind 3's box (0x201E18): the particles kept in the position record's box around the camera
            if (kind == 3 && st.rec.pos.k == 1) WrapPepsysInBox(st.ppsys, matFrame, st.rec.pos.v0, st.rec.pos.v1);
            SetPepsysDraw(st.ppsys, false, amatDraw);
            ++g_pestats.cstream;
            g_pestats.cparticle += CParticlesPepsys(st.ppsys);
        }
        ++g_pestats.cinst;
        if (static const bool s_fList = std::getenv("SLY3_PELIST") != nullptr; s_fList && s_cUpdate == 60)
        {
            const glm::vec3 p = PosLoWorld(ppe);
            std::printf("pelist %d kind %d at %.0f %.0f %.0f d %.0f clones %zu emit %d:", ppe->oid, kind, p.x, p.y, p.z, glm::distance(p, s_posCam), amatDraw.size(), int(fEmit));
            for (const STREAM& st : inst.astream)
                std::printf(" [shd %d k%d n%d rgba %08X]", st.rec.desc.shd, st.rec.desc.kDraw, CParticlesPepsys(st.ppsys), st.rec.desc.argba.empty() ? 0u : st.rec.desc.argba[0]);
            std::printf("\n");
        }
        // debug: SLY3_PEOID=oid (the prototype's for kind 2): its records and particles once a second
        static const int s_oidDbg = std::getenv("SLY3_PEOID") ? std::atoi(std::getenv("SLY3_PEOID")) : -2;
        static int s_cDbg = 0;
        if (pkey->oid == s_oidDbg && s_cDbg++ % 60 == 0)
        {
            const glm::vec3 p = PosLoWorld(ppe);
            std::printf("peoid %d kind %d at %.0f %.0f %.0f clones %zu emit %d radius %g\n", ppe->oid, kind, p.x, p.y, p.z, amatDraw.size(), int(fEmit), ppe->sRadius);
            for (const STREAM& st : inst.astream)
            {
                const PEREC& r = st.rec;
                std::printf("  comp %d: pos k%d n%d v0 %.1f %.1f %.1f v1 %.1f %.1f %.1f dir %.2f %.2f %.2f | speed %.0f..%.0f tilt %.2f..%.2f life %.2f drag %.2f accel %.0f %.0f %.0f"
                            " | scale %.1f %.1f %.1f alpha %.2f %.2f %.2f | draw k%d fly %g upd k%d shd %d colours %zu pairs %d | cmd %d/%zu loop %d done %d rate %.1f | particles %d\n",
                            st.pcomp->oid, r.pos.k, r.pos.kNormal, r.pos.v0.x, r.pos.v0.y, r.pos.v0.z, r.pos.v1.x, r.pos.v1.y, r.pos.v1.z, r.pos.dir.x, r.pos.dir.y, r.pos.dir.z,
                            r.vel.speedLo, r.vel.speedHi, r.vel.tiltLo, r.vel.tiltHi, r.vel.life, r.vel.gDrag, r.vel.accel.x, r.vel.accel.y, r.vel.accel.z,
                            r.vel.clqScale[0], r.vel.clqScale[1], r.vel.clqScale[2], r.vel.clqAlpha[0], r.vel.clqAlpha[1], r.vel.clqAlpha[2], r.desc.kDraw, r.desc.gFlying,
                            r.desc.kUpdate, r.desc.shd, r.desc.argba.size(), int(r.desc.fColorPairs), st.icmd, r.acmd.size(), st.cLoop, int(st.fDone), st.gRate,
                            CParticlesPepsys(st.ppsys));
                for (const PECMD& c : r.acmd) std::printf("    cmd k%d %.2f %.2f dur %.2f\n", c.k, c.lo, c.hi, c.dur);
                for (uint32_t u : r.desc.argba) std::printf("    rgba %08X\n", u);
            }
        }
    }
    // the one-shot effects (kind 0): their commands run once (the frame object's matrix each frame when it has one),
    // the instance dropped when its streams are done and their particles gone (0x1FEF60's flag 2: slot 5)
    for (auto it = s_aoneshot.begin(); it != s_aoneshot.end();)
    {
        ONESHOT& os = *it;
        if (os.ploFrame) os.matFrame = dynamic_cast<const LOX*>(os.ploFrame) ? MatWorldOfPe(os.ploFrame) : MatWorldOfAlo(static_cast<const ALO*>(os.ploFrame));
        bool fAlive = false;
        for (STREAM& st : os.inst.astream)
        {
            FRunCommands(st, os.inst.gScale, os.matFrame, glm::vec3(0.0f));
            SetPepsysDraw(st.ppsys, false, {});
            fAlive = fAlive || !st.fDone || CParticlesPepsys(st.ppsys) > 0;
        }
        if (!fAlive)
        {
            for (STREAM& st : os.inst.astream) SetPepsysDraw(st.ppsys, true, {});
            it = s_aoneshot.erase(it);
        }
        else ++it;
    }
    if (s_fDbg)
    {
        static int s_c = 0;
        if (s_c++ % 60 == 0)
            std::printf("pe: %zu objects, %d instances, %d streams, %d particles\n", psw->mpcidaplo[CID_PE].size(), g_pestats.cinst,
                        g_pestats.cstream, g_pestats.cparticle);
    }
}

// orig3: 0x1FE840 (a kind-0 effect's instance for the frame object, made when there is none) + its start: the effect
// plays once at the frame object (null: the effect object's own place). The scripts / damage code call it.
void TriggerPe(LO* ppe, const LO* ploFrame)
{
    if (!ppe || !g_psw) return;
    if (s_gen != GenPeParticles() || s_psw != g_psw || s_serial != g_psw->serial)
    {
        ResetPes();
        s_gen = GenPeParticles();
        s_psw = g_psw;
        s_serial = g_psw->serial;
    }
    const std::vector<PECOMP>* pacomp = PacompOf(ppe);
    if (!pacomp) return;
    ONESHOT os;
    os.inst.ppe = ppe;
    os.inst.kind = 0;
    os.ploFrame = ploFrame;
    os.matFrame = ploFrame ? (dynamic_cast<const LOX*>(ploFrame) ? MatWorldOfPe(ploFrame) : MatWorldOfAlo(static_cast<const ALO*>(ploFrame))) : MatWorldOfPe(ppe);
    s_ploNewFor = ppe;
    for (const PECOMP& comp : *pacomp)
        if (comp.cid == 0xBF || comp.cid == 0xC0) os.inst.astream.push_back(StreamNew(comp));
    if (std::getenv("SLY3_PEDBG"))
    {
        const glm::vec3 p = glm::vec3(os.matFrame[3]);
        std::printf("pe trigger %d (kind %d) frame %d at %.0f %.0f %.0f:", ppe->oid, NOpt(ppe, 539, 0), ploFrame ? ploFrame->oid : -1, p.x, p.y, p.z);
        for (const STREAM& st : os.inst.astream)
            std::printf(" [0x%X %d: rip kind %d model %d (%s) size %.0f..%.0f, %zu commands]", st.pcomp->cid, st.pcomp->oid, st.rec.kRip, st.rec.oidModel,
                        st.pmodel ? "found" : "none", st.rec.sizeLo, st.rec.sizeHi, st.rec.acmd.size());
        std::printf("\n");
    }
    s_aoneshot.push_back(std::move(os));
}

// The loader's registration (loaders.cpp RegisterLoaders) and the native struct (PfnNewNativeForCid)
void RegisterPeLoaders(std::unordered_map<uint32_t, PFNLOAD>& m) { m[0x1FE270] = LoadPeFromBrx; }
PFNNEW PfnNewPe() { return [] () -> LO* { return new LOPE(); }; }

namespace
{
// the world update's 0x202D80 (natively in the particle pass 0x1F8DD8, after emitter.cpp's: the systems move there)
struct INITPE
{
    INITPE() { RegisterSwPass(0x1F8DD8, [](SW* psw) { UpdatePes(psw); }); }
} s_initpe;
}  // namespace
