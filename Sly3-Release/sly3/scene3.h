#pragma once

// The renderer's standalone scene of a Sly 3 world (owner: the Sly 3 renderer), until the class loaders (the Sly 3 class loaders)
// create the objects: every group's textures (LoadShadersFromBrx 0x234EA0, the real loader) and, for every object
// whose class derives from ALO (0x2F) and whose loader starts with LoadAloFromBrx 0x23C0B8 (all but six ALO classes:
// tools of the research, scratch analysis), the head of that loader: the transforms (LoadAloXfsFromBrx 0x28C338 with
// the pairs 0x1A5D10, links 0x182808, chains 0x26EF10 and three S16 references), the U32, the glob set (0x18D148).
// The rest of the body is skipped by its size (so children of objects are not reached yet). Objects of mode 1 load
// only when their filters (world indices) and conditions pass, as 0x151D80 does.
//
// Each object is placed by its own transform records (record 0, the references inside the object): static objects
// sit where their file puts them. --view-world3 draws the scene with the copied GL renderer.

#include "brx3.h"
#include "../scene.h"

#include <memory>
#include <string>
#include <vector>

struct SCENE3STATS
{
    int cgroup = 0, cgroupShd = 0, cobj = 0, cobjAlo = 0, cobjFiltered = 0, cglobset = 0, cglobsetEmpty = 0;
    int cfail = 0;            // objects whose ALO head did not parse (the first errors are printed)
    int cnotFirst = 0;        // ALO-derived classes whose loader does not start with 0x23C0B8 (skipped)
    int cproxy = 0, cproxyNoGroup = 0, cplaced = 0;  // group instances placed by proxies; scene entries
    int cgroupNoInst = 0;     // groups the chosen option-set block gives no instance (EOPID 1032): not loaded
    int cpoolRam = 0;         // pool instances placed from a RAM dump (g_pramPools3)
    int cgroupPool = 0;       // groups of their own namespace (+0x5C 0) no proxy places: pools spawned by code, not drawn
    std::vector<std::string> aerr;
    // per scene entry: its transform records' world matrices as placed (the file / proxy pose), for CApplyRamPose3
    std::vector<std::vector<glm::mat4>> aamatXf;
};

// The group instance counts as the loader sees them (0x14F760 skips a group whose +0x08 is 0): the walk's record value,
// overridden by the options EOPID 1032 (kind 0x12, a CRef pair {group id = record +0x10, count}; setter 0x10AFB0 ->
// 0x169500) of the option-set block (0x1500F0) the world loads into the condition set: the block whose id is the
// current condition (m_ext: blocks 13 / 14 / 15; 13 at the prologue gives Murray, Carmelita, the Guru, the Panda King,
// Penelope, the gator crab, the ape, ... 0: the 17 glob sets the reference never reads). Interface for the world
// loader (the Sly 3 class loaders): apply these counts before the group loop.
constexpr int EOPID_GROUP_INSTANCES3 = 1032;
// Debugging (--ram3-pools): a PCSX2 EE RAM dump whose live instances of the pools (groups of their own namespace no
// proxy places: spotlights, missiles, ... spawned by objects not ported yet) are placed where the RAM has them (their
// ALO's XF, +0x44: +0x40 the world rows, +0x70 the position).
extern const std::vector<uint8_t>* g_pramPools3;
std::vector<int> AcinstGroups3(const WORLDWALK3& w, uint32_t idBlock);

// orig3: 0x23FE38 (fShow) / 0x23FEC0 ShowAloGlobs: every group of the ALO's glob set (ALO +0xA8 count, +0xB0 the
// 0x28-byte groups) whose n0 (+0) is n0 gets its hidden flag (+0xC 0x1000) cleared / set (and the bit 50 of its
// +0x24 record's +0x10 cleared / set); RenderGlob 0x18EBEC skips a hidden group. Callers: scripts (0x13F440), the
// options / actors (0x152488 the players, 0x1C8970 BindLoParts, 0x25D810 ...). The scene's equivalent: the entry's
// afVisible. Returns the number of groups changed. Interface for the object code (the Sly 3 class loaders / the Sly 3 script port).
int ShowSceneGlobs3(SCENEGLOBSET& sgs, int32_t n0, bool fShow);

// The world's collision as placed (the Sly 3 player): every collision entry (SO / ALO entries, orig3 0x23C0B8: S16
// count x {S16 record, S32 tag, S8, S8, F32 radius, S8, the GEOM, the BSP, U32 id, U16, S8, S16, S8}, the cmesh readers
// 0x117FD0 / 0x118220 as Sly 2's) of every object the scene places, its GEOM in world space by its record's placed
// world matrix (the original's 0x166270 / 0x1664A0 on the entry's record), its BSP over that GEOM. Filled by
// FBuildScene3 (also for objects that draw nothing); the player's floor / walls query it (lo.h SOE: geomWorld, bsp).
struct SCENECOLL3
{
    int oid = -1, cid = -1;
    std::unique_ptr<struct SOE> psoe;
};
extern std::vector<SCENECOLL3> g_acoll3;

// Walks the decompressed world ab (w: its --dump-world walk) into scene. false on a texture-block error.
bool FBuildScene3(const std::vector<uint8_t>& ab, const WORLDWALK3& w, SCENE& scene, SCENE3STATS& st, std::string& error);

struct VIEW3OPTS
{
    std::string world = "m_ext";
    std::string ram;          // a PCSX2 EE RAM dump: the condition set, the filter, and the camera (CM 0xAC)
    std::string screenshot;   // one frame to this PNG
    std::string camera;       // x,y,z,yaw,pitch
    int width = 1280, height = 960;
    bool fNoCel = false, fNoFog = false;
    bool fRamPools = false;
    int nRamHud = 0;          // --ram3-hud slots (1: the RAM's slots) | native (2: the native keys at the RAM's parts / times)
    bool fRamPose = false;    // --ram3-pose: CApplyRamPose3   // --ram3-pools: place the pools' live instances from the --ram3 dump (g_pramPools3)
};
int ViewWorld3(const VIEW3OPTS& vo);

// Debugging (--ram3-pose): the objects whose oid is a single ALO of a PCSX2 EE RAM dump (the players, ...) take its
// pose: every transform record's world matrix from RAM (ALO +0x40 count, +0x44 records of 0x100 bytes: +0x40 the
// world rows, +0x70 the position), the groups' matrices from their owner records, and the skinned groups' bone
// matrices (GLOB::abone {oid, record}: the bone record's RAM world x the inverse of its placed world x the owner's
// placed world, as BindAloBones 0x167B68 / FGlobBoneMatrices 0x1684D0 bind them). Returns the entries posed.
int CApplyRamPose3(SCENE& scene, const SCENE3STATS& st, const std::vector<uint8_t>& ram);
