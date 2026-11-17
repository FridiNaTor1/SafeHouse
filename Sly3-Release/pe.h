#pragma once

// Sly 3's particle effects (class 0x2D "PE", new in Sly 3; module 0x1FC000..0x203400, no Sly 2 twin): the torch flames,
// the rain, the fireflies, the drips and sparks of the world ("pe_*", "psys_*", "xps_*", "explode_*" in the oid names).
//
// A PE object (0x100 bytes, vt 0x361258, loader 0x1FE270) is a LOX with a kind (+0xD8, option 539: 0 a one-shot
// effect, 1 a particle system at the object, 2 an effect shared by the object's clones, 3 a camera-attached one: the
// rain), lock flags (+0xDC, 543), two objects (+0xE0 / +0xE4, 541 / 542: the emission frame and, for kind 3, the
// particles' frame; InitPe 0x1FE238 sets both to the object) and components (+0xD0 / +0xD4): class 0xBF (a particle
// stream: 0x200868, vt 0x3615B8) or 0xC0 (a RIP stream: 0x202220, vt 0x361578), each an option list on its records
// (position 0x60 bytes, velocity / life 0x90, shared 0x30 -> render 0x220) and a command list (EOPIDs 552..556: burst, rate,
// sound, stop, wait; 557 the repeat count) that the stream's runner (0x1FF100) plays.
//
// The streams are particle systems of emitter.cpp (orig3 0x200C10 / 0x200F50 are Sly 2's 0x15B428 / 0x139DC0 on the
// same blocks): pe.cpp chooses the particles (0x1FF6B0 positions, 0x1FFC20 velocities, 0x2001C0 times, 0x200328 the
// frames) and runs the instances (0x202A88, the world's 0x202D80 / 0x202E58).

#include "lo.h"

#include <memory>
#include <vector>

struct SW;

// A component (0x20-byte record made by 0x1FD758 (0xBF) / 0x1FDE70 (0xC0)).
struct PECOMP
{
    int cid = 0;                 // 0xBF / 0xC0
    int32_t oid = 0;             // +4
    int n = 1;                   // +8: the command list's repeat count (0 / 1: once, < 0: forever); 1 from 0x1FCCE8,
                                 // option 557 sets it (the item header's S16 is read and dropped by the loader)
    std::vector<OPT> aopt;       // its options (EOPIDs 553..606)
    std::shared_ptr<LOXEMIT::EMITMESH> pmesh;  // the emission mesh (position kind 3: 0x1FCF10)
};

struct LOPE : LOX
{
    std::vector<PECOMP> acomp;   // +0xD0 count, +0xD4 (the items whose filters / conditions pass)
};

// The world's effects each frame (the world update's 0x202D80); the camera the emission tests (0x203170) use.
void UpdatePes(SW* psw);
void SetPeCamera(const glm::vec3& pos, const glm::mat3& matRows, const glm::mat4& matWorldToClip);
// (PC split screen, orig3 0x18E728: a PE is tested against every view's camera) the other views' camera positions;
// c 0: none
void SetPeCamerasExtra(const glm::vec3* aposCam, int c);   // (the drawn camera: rows view, left, up)
// orig3: 0x1FE840 + the start: a one-shot (kind 0) effect played once at ploFrame (null: at the effect object). For the
// scripts / damage code (e.g. the attract's lightning xps_m_lightning1 / 2, the explode_* effects).
void TriggerPe(LO* ppe, const LO* ploFrame = nullptr, bool fOwnRandom = false);
// (PC) the random draws of the effects code: GRandInRange, or the breakable effects' own stream while one runs
float GRandPe(float lo, float hi);
void SetPeOwnRandom(bool f);   // (the interaction sparkles, jt.cpp: their draws on the same own stream)
// orig3: 0x1FE840(ppe, ppe, 0, 1) + the stream's slot 2: c particles given (world positions / velocities, birth / end
// times) into a kind-0 effect's stream, its size curve (the velocity record's +0x20) set to pclqScale when given (the
// interaction sparkles 0x1B6B10)
struct PEVELPARAMS { float speedLo = 0, speedHi = 0, advLo = 0, advHi = 0, ageLo = 0, ageHi = 0, life = 1; };
bool FPeVelParams(const LO* ppe, PEVELPARAMS* pp);
void EmitPeParticles(LO* ppe, int c, const glm::vec3* apos, const glm::vec3* avel, const float* at0, const float* at1, const float* pclqScale);
// orig3: 0x25D940 (the scripts' 0x41, the ASEG event 17, a breakable's item 7): the effect oidEffect found near
// ploContext: a PE started, else an explosion (from oidSource, else itself; scaled when g != 1)
void StartEffectByOid3(int oidEffect, int oidSource, float g, LO* ploContext, bool fOwnRandom = false);
// A PE whose kind (+0xD8, option 539) is 0: a one-shot, out of the world from its start (0x1FE5E0)
bool FPeOneShot(const LO* plo);
// Statistics / debugging (SLY3_PEDBG=1 prints the instances).
struct PESTATS
{
    int cpe = 0, ccomp = 0, cbad = 0, cinst = 0, cstream = 0, cparticle = 0;
};
extern PESTATS g_pestats;
