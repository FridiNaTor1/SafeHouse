#pragma once

// Particles, natively: the emitters (class 0x1D, Sly 1's emitter.c; module 0x159118..0x15C690) and the particle
// systems they emit into (class 0x1C, vtable 0x2CB5D8, module 0x138FF8..0x13A9E8: Sly 2's "blips", drawn as render
// kind 12 by VU1 programs 0x25A0C0..0x25AF90).
//
// The emitter keeps the original's parameter records as byte images (EMITTER +0xE0..+0x160, its EMITB 0x100 bytes)
// so the loaded options (their getters' sub-records and field offsets, EOPIDs 13 / 469..533) apply exactly as
// LoadOptionFromBrx 0x13E818 writes them, and the ported code reads the same offsets. Object references (EMITB +8:
// the EMITS of 0x2586D8) are native pointers beside the images.
//
// The game loop: emitters update through their class update (RegisterLoUpdate 0x159C80, UpdateEmitter), the
// particle systems move in the world's pass 0x1F8DD8 (their vtable slot 17, 0x139DC0) and are drawn at render
// kind 12 (RenderScene calls the hook below).

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <vector>

struct SW;
struct LO;
struct RENDEROPTS;

// The particle drawing for the frame pass (render.cpp calls it where kind 12 is drawn).
void DrawParticles(const glm::mat4& matWorldToClip, const glm::vec3& posCamera, float aspect);

// Statistics for tests.
struct PARTICLESTATS
{
    int cemitter = 0, cemitterGroup = 0, cpsys = 0, cparticle = 0, cdrawn = 0, cripSkipped = 0, cupdate = 0, cexpl = 0, cburst = 0, crip = 0;
};
extern PARTICLESTATS g_particlestats;

// Builds the emitters of the world (options, 0x2586D8 + PostEmitterLoad 0x159500); called by the first update.
void InitEmittersOfSw(SW* psw);
// (PC, pcloader.h: objects made while the world runs, e.g. a kept world's copies) their emitters and explosions built as
// the world's init builds them; nothing when the world's emitters are not built yet (its init takes them then)
void AddEmittersOfNewLos(SW* psw, const std::vector<LO*>& aplo);
// Drops every emitter and particle system (a new world).
void ResetParticles();
// Runs n frames of emitters and particle systems at dt (the viewer: every emitter counts as active).
void StepParticles(SW* psw, int cframe, float dt);
// The camera the emitters cull against (the game loop's / viewer's).
void SetParticleCamera(const glm::vec3& pos, const glm::vec3& dirView, const glm::vec3& dirLeft);
// For the actors (jt.cpp 0x195368): c particles of the emitter object plo (class 0x1D) put into its particle system
// at the given places (0x15B428); false without a native emitter. GEmitterLo: a float of its EMITTER (fEmitb false)
// or its EMITB at the original's offset.
struct LO;
bool EmitLoParticlesAt(LO* plo, int c, const glm::vec3* apos, const glm::vec3* avel, const float* at0, const float* at1);
float GEmitterLo(LO* plo, bool fEmitb, int off);
// Prints the emitters (oid, position, kinds, shader) for finding them.
void DumpEmitters();

// An explosion's burst (orig 0x15C690 with its parameter block): grf 1 the source object, 2 the direction (EMITB
// +0x40), 4 the position (a point), 8 the density (g8, else gC / 100), 0x10 the RIPs' size (g8), 0x20 the RIPs'
// touched object, 0x40 a frame matrix, 0x80 the count.
struct BURSTPARAMS
{
    uint32_t grf = 0;
    LO* plo = nullptr;              // +4
    float g8 = 0, gC = 0;           // +8 / +0xC
    glm::vec3 dir{0.0f};            // +0x10
    glm::vec3 pos{0.0f};            // +0x20
    LO* ploTouch = nullptr;         // +0x30
    glm::mat4 matFrame{1.0f};       // +0x34
    int c = 0;                      // +0x38
};
// For the splice / world code: explodes an explosion object (class 0x20 family: its burst; a group: each of
// its explosion children), or every explosion with the oid.
void ExplodeLo(LO* plo, const BURSTPARAMS* pparams = nullptr);
// (PC addition, debug menu only: pcloader.cpp sets them) a copy of a kept world's emitter: its shader from that world's
// shaders, the objects it names (its source, models) from that world (null: the running world's as before)
struct SHD;
extern const SHD* (*g_pfnPcShdOfLo)(const LO* plo, int32_t id);
extern LO* (*g_pfnPcFindOid)(const LO* ploNear, int32_t oid);
void ExplodeOid(SW* psw, int32_t oid, const BURSTPARAMS* pparams = nullptr);

// A kind-9 RIP's definition (the original's +0xF0 record, e.g. the coin's D_002C0DA0): the goal in the camera's screen
// space (x right / y up over the screen ranges, z the depth) and the velocity there, the start velocity's range, the
// spin axis, the size polynomial of the age
struct RIPHUDDEF
{
    glm::vec3 posGoal{0.0f}, velGoal{0.0f};   // +0x0 / +0x10
    glm::vec3 velLo{0.0f}, velHi{0.0f};       // +0x20 / +0x30
    glm::vec3 axisSpin{0.0f, 0.0f, 1.0f};     // +0x40
    float size[3] = {1.0f, 0.0f, 0.0f};       // +0x50
};
// orig: 0x001CE888(G, 9, 0) + vt slot 1 0x1D04E8 (a pickup's flight to the HUD: the model drawn from posWorld with
// its world orientation, along a Hermite curve in screen space to the goal in 0.5 s, turning to the camera's
// orientation and spinning once, 0x1D05A0 / 0x1D0630; at its end the callback, 0x1D0800). False: no RIP (the caller
// does the callback's work at once).
bool FLaunchHudRip(const LO* pmodel, const glm::vec3& posWorld, const glm::mat3& matWorld, const RIPHUDDEF& def, std::function<void()> pfnArrive);

// The scene's entry of an object, and its globs drawn at mat (its object record's matrix) with an alpha as extra draws
// (0x1A5EE8; render.h EXTRADRAW)
struct SCENE;
struct SCENEGLOBSET;
struct ALO;
struct EXTRADRAW;
const SCENEGLOBSET* PsgsOfAlo(const SCENE& scene, const ALO* palo);
void AppendAloExtraDraws(const SCENE& scene, const ALO* palo, const glm::mat4& mat, float alpha, std::vector<EXTRADRAW>& aextra);
// orig: 0x001CE888(G, 4, 0) + 0x1D0108 / 0x1D0158 / 0x1D0190 (a pickup's glint, 0x1C6308: pmodel drawn at ploAt's point
// facing the camera for dt seconds, growing and fading)
bool FLaunchGlintRip(const LO* pmodel, const LO* ploAt, float dt);
// A water ripple (WakeSoWater 0x15A2C0's 0x1CE888(G, 7, 0)): a plain RIP (kind 7, vtable 0x2D7788, the base slots:
// 0x1CE920's start, the default alpha 0x2C1F50, 1 s) of the water's ripple model at pos, its size gScale growing by
// 0.6 gScale over its life.
bool FLaunchWaterRipple(const LO* pmodel, const glm::vec3& pos, float gScale);
// orig: 0x0015BE28 from WakeSoWater (the wake's spray: c kind-1 RIPs of ploSpray falling back into the water ploWater
// with ripples of ploRipple; d the relative speed, accel the object's gravity)
void LaunchWaterSpray(const LO* ploSpray, const LO* ploRipple, LO* ploWater, const glm::vec3& pos, float d, const glm::vec3& accel, int c);

// Sly 3's particle effects (class 0x2D, pe.cpp): a stream (orig3 0x200868, vt 0x3615B8) is a particle system of this
// module (the same blocks, update 0x179CC0 = 0x139DC0 and VU1 draw) set up from the PE component's records.
struct PEPSYS;
struct PEPSYSDESC
{
    int kDraw = 0;                  // shared +0x24 (0 billboard, 1 flying, 2 rolling)
    float gFlying = 1.0f;           // shared +0 (kind 1)
    std::vector<uint32_t> argba;    // render +0x21C (count +0x218)
    bool fColorPairs = false;       // render +0x214
    int32_t shd = 0;                // render +0 (EOPID 584)
    float life = 1.0f;              // velocity +0x78 (577)
    float clqAlpha[3] = {1, 0, -1}; // velocity +0x30 (580)
    float clqScale[3] = {25, 0, 0}; // velocity +0x20 (581)
    float dTex = 0.0f;              // shared +0x1C (586)
    bool fAlphaOne = false;         // shared +0x18 (588)
    int kUpdate = 0;                // shared +0x28 (583)
    glm::vec3 accel{0, 0, -1500};   // velocity +0 (572)
    glm::vec3 axisSpin{0, 0, 1};    // velocity +0x10 (574)
    float gSpin = 0.0f;             // velocity +0x70 (573)
    float gDrag = -1.0f;            // velocity +0x68 (571)
    bool fRandTex = false;          // shared +0x14 (585)
    bool fSpinFixed = false;        // shared +0 (kind 2, 590)
    bool fRoll = false;             // shared +4 (591)
    float spinLo = 0, spinHi = 0;   // shared +8 (592)
};
uint64_t GenPeParticles();          // changes when the particle systems are dropped (the handles below are then stale)
PEPSYS* PpepsysNew(const PEPSYSDESC& d);
void SetPepsys(PEPSYS* pp, const PEPSYSDESC& d);
void EmitPepsys(PEPSYS* pp, int c, const glm::vec3* apos, const glm::vec3* avel, const float* at0, const float* at1);
int CParticlesPepsys(const PEPSYS* pp);
void SetPepsysDraw(PEPSYS* pp, bool fHidden, const std::vector<glm::mat4>& amat);
void WrapPepsysInBox(PEPSYS* pp, const glm::mat4& mat, const glm::vec3& lo, const glm::vec3& hi);
// A PE RIP stream's particle (orig3 0x202330, pe.cpp): a RIP of the model at pos (kind: the component's record +0x1C)
struct PERIPDESC
{
    int kind = 3;
    const LO* pmodel = nullptr;     // +0x1C (the record's +0xC object)
    const LO* plo2 = nullptr;       // the record's +0x10 object (the kinds' second object)
    glm::vec3 pos{0.0f}, vel{0.0f}, accel{0.0f}, axisSpin{0, 0, 1};
    float tBirth = 0, life = 1;
    float size[3] = {1, 0, 0}, alpha[3] = {1, 0, 0};
    float gSpin = 0, gDrag = 0;
    glm::mat3 ori{1.0f};
};
void LaunchPeRip(const PERIPDESC& d);
