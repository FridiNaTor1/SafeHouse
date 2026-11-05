#pragma once

// Sly 3's animation runtime on the native objects (the Sly 3 animation): the tracks (trk3.h) under the copied ASEGA / XF
// runtime (asega.cpp, xf.cpp), its world passes, and the checks against the reference's RAM.

#include <cstdio>

struct SW;
struct SCENE;
struct SCENE3STATS;
struct ALO;
struct ASEG;
struct ASEGA;
struct LO;
#include <cstdint>

namespace sly3
{
// --aseg-check3 FILE (with --load-world W): every line of tools/asegram3.py (an ASEGA's channel and the segment its
// record holds in the RAM) set up natively from the same ASEG / channel at the same time and compared. Returns the
// count of mismatches.
int CheckAsegSegments3(SW* psw, const char* pszFile, FILE* pf);

// The world start's animation part (game3 FLoadWorld3, after StartSw's parts): the records' run-time state (InitAloXfs),
// the ASEG / SM tables, then each ASEG's / SM's PostLoad in load order (an ASEG with the apply parameters EOPID 113
// {t, svt, grf} applies itself, orig3 0x1367F0 as Sly 2's 0x12F8F0; an SM with EOPID 414 {oid, state, grfapl} starts
// its SMA). It registers the frame pass (once). SLY3_NOANIM=1 leaves the world unanimated.
void StartWorldAnimation3(SW* psw, SCENE* pscene, const SCENE3STATS* pst);
// One frame (the world update 0x27D7E8's animation part, as Sly 2's 0x226550: the ASEGAs (UpdateAsega 0x137900),
// then the records of the objects they drive (UpdateAloXfs 0x28CA78), their world matrices (0x28E768)); then the
// scene entries of those objects take their records' world matrices and bones (CApplyNativePose3).
void UpdateWorldAnimation3(float dt);
// The scene entries of animated native ALOs: their globs' and bones' matrices from the records (as CApplyRamPose3
// does from a RAM dump). Returns the entries posed.
int CApplyNativePose3(SCENE& scene, const SCENE3STATS& st, SW* psw);

// For the actors (the Sly 3 player): an ASEG of the world by oid near an object (PloFindSwNearest) and the copied
// runtime's entry points (asega.h: ApplyAsegCur, EnsureAseg, RetractAsegaPp, SetAsegaSpeed, ...).
ASEG* PasegFind3(SW* psw, int oid, ALO* paloNear);
// The players' animation fields (actoraseg3.inc: Sly 0x34 / Bentley 0x36, from the reference's RAM): the field at
// offset ib of the original's actor names an ASEG / blend / SM; PloActorAnimField3 gives the native object (found by
// its oid around the actor; null for the blends 0x83, which NewBlend makes, and unknown offsets).
struct ACTORANIM3
{
    int cidActor;
    uint32_t ib;
    int cid;   // 0x82 ASEG, 0x83 blend, 0x84 SM
    int oid;
};
const ACTORANIM3* AactoranimBegin3(int* pc);
LO* PloActorAnimField3(ALO* pactor, uint32_t ib);
// An ALO's scene entries shown / hidden (AddLo3 / RemoveLo3, trigger3.h: an object a task removes is not drawn; its
// entries' own per-glob visibility comes back when it is added)
void ShowAloEntries3(ALO* palo, bool fShown);
// Counts for the reports: ASEGAs playing, records driven
int CasegaPlaying3();
}  // namespace sly3
