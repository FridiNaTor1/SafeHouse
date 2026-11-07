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
    int n = 0;                   // +8: the command list's repeat count (0: once, < 0: forever)
    std::vector<OPT> aopt;       // its options (EOPIDs 553..606)
    std::shared_ptr<LOXEMIT::EMITMESH> pmesh;  // the emission mesh (position kind 3: 0x1FCF10)
};

struct LOPE : LOX
{
    std::vector<PECOMP> acomp;   // +0xD0 count, +0xD4 (the items whose filters / conditions pass)
};

// The world's effects each frame (the world update's 0x202D80); the camera the emission tests (0x203170) use.
void UpdatePes(SW* psw);
void SetPeCamera(const glm::vec3& pos, const glm::mat3& matRows, const glm::mat4& matWorldToClip);   // (the drawn camera: rows view, left, up)
// orig3: 0x1FE840 + the start: a one-shot (kind 0) effect played once at ploFrame (null: at the effect object). For the
// scripts / damage code (e.g. the attract's lightning xps_m_lightning1 / 2, the explode_* effects).
void TriggerPe(LO* ppe, const LO* ploFrame = nullptr);
// Statistics / debugging (SLY3_PEDBG=1 prints the instances).
struct PESTATS
{
    int cpe = 0, ccomp = 0, cbad = 0, cinst = 0, cstream = 0, cparticle = 0;
};
extern PESTATS g_pestats;
