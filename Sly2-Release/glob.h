#pragma once

// Meshes (Sly 2 module 0x167310-0x16EFE0, Sly 1's glob.c): LoadGlob 0x167320 and its four mesh loaders, read into
// native vertex/edge arrays instead of the VU1 DMA chains the original builds.
//
// Names follow Sly 1 where the thing is the same: GLOBSET is the mesh of an ALO (Sly 2's glob at ALO+0x80), a GLOB
// is one of its groups (Sly 2 "header" 0x14 + "group" 0x28 + draw record 0x18; Sly 1 GLOB: rp, gZOrder, uFog,
// rtck, MRD, posCenter/sRadius), a SUBGLOB a run of strips of one shader (a type-0 batch entry).
// Instances ("sub-meshes", 0x169AC8) are GLOBs that share their template's geometry with their own matrix and
// colours.

#include "bis.h"
#include "shd.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

struct ALO;
struct SAA;

enum MESHK : int8_t
{
    MESHK_Strips = 0,       // 0x16A000: vertex pool + strips
    MESHK_CelBorder = 1,    // 0x16E438: cel-border edges (Sly 1 SUBCEL)
    MESHK_Effect = 2,       // 0x16CAA0: screen-effect mesh (positions + 4-byte records)
    MESHK_Indexed = 3,      // 0x16CF78: screen-effect mesh, indexed positions + strip mask
};

// A bone binding of a group (0x50 bytes at group+0x18: {oid, plo, ibone, pbone, mat}; bound by 0x167B68).
struct GLOBBONE
{
    int32_t oid = 0;
    int16_t ibone = 0;
};

// One strip vertex of a type-0 mesh, in the units the VU program receives: colours as GS 0..255 (0x80 = 1.0).
struct VTXGLOB
{
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
    uint32_t rgbaA = 0;     // qw 1: the first pass's colour (RGB5A1 expanded, alpha byte)
    uint32_t rgbaB = 0;     // qw 2: the second colour (lit / table colours)
    uint8_t abone[4] = {};  // indices into the group's bone list
    uint8_t aw[4] = {};     // raw weight bytes
    uint16_t isrc = 0;      // source pool index (for instance recolouring)
    uint32_t rgbaSrc = 0;   // the pool's (adjusted) vertex colour, the lighting programs' input
    uint8_t fAdc = 0;       // strip mask bit: no triangle ends here
    uint8_t fUnbaked = 0;   // lit shader without table colours: rgbaA is the light bake's input (vertex x material)
};

// A type-0 entry: count strip vertices with one shader (12 bytes {count, shader index, flags} in the stream).
struct SUBGLOB
{
    SHD* pshd = nullptr;
    SAA* psaa = nullptr;            // the entry's animation (+0xC: the group's, else the shader's, if it drives the shader)
    uint32_t grf = 0;               // entry flags (+8): 1/2/4 lanes, 0x20 colours from the index table
    int ibatch = 0;
    uint32_t ivtxFirst = 0;         // first strip vertex in GLOB::avtx
    uint32_t cvtx = 0;
    uint32_t firstIndex = 0;        // triangles in GLOB::aindex
    uint32_t indexCount = 0;
    // Renderer cache (render.cpp): the passes' GL states for one shader / SAA frame (checked against its owner, so
    // a copied SUBGLOB builds its own).
    mutable struct SUBGLOBGPU* pgpu = nullptr;
};

// A cel-border strip (type 1): positions, weights and edges {E0, E1, OA, OB} (Sly 1 SUBCEL / TWEF).
struct SUBCEL
{
    std::vector<glm::vec3> apos;
    int cw = 0;
    uint8_t aiboneSlot[4] = {};
    std::vector<uint8_t> aw;        // cv * cw
    std::vector<uint8_t> aedge;     // cn * 4
};

// A screen-effect strip (types 2 and 3).
struct SUBFX
{
    SHD* pshd = nullptr;
    std::vector<glm::vec3> apos;    // type 2: cv; type 3: the position dictionary
    std::vector<uint8_t> aipos;     // type 3: per strip vertex
    std::vector<uint32_t> amask;    // type 3
    std::vector<uint8_t> arec;      // type 2: cn * 4 (edges {OA, E0, E1, OB} as vertex index x 2, like the cel borders)
    uint8_t aiboneSlot[4] = {};     // type 2: the weights' bones (GLOB::abone indices)
    int cw = 0;
    std::vector<uint8_t> aw;
};

// Instance colour sets (0x16C8E0): {c sets {n records {u16 colour A, u16 colour B, u8 alpha}}, ai[batch] -> set}.
struct IDXTAB
{
    struct REC { uint16_t rgba16A; uint16_t rgba16B; uint8_t a; };
    std::vector<std::vector<REC>> aset;
    std::vector<int32_t> aiset;
};

// A warp block of a type-0 glob (0x16A000, mesh flag 4; Sly 1's WRBG, orig 0xA4 bytes): the WR (cid 0x73) its oid
// names, the envelope of its vertices' weights (WEKI: the weight uInner at sInner to uOuter at sOuter along wek's
// measure in dmat's space), its matrices' count and kinds; bound by 0x167B68 / 0x22B7A0, its state vectors made by
// 0x22B7D8 (wr.cpp) and applied by glob.vert
struct WRBG
{
    int32_t oid = -1;                // +0x08
    int wek = -1;                    // +0x30 (-1: every weight 1)
    float sInner = 0.0f, uInner = 0.0f, sOuter = 0.0f, uOuter = 0.0f;
    glm::mat4 dmat{1.0f};            // +0x50
    int cmat = 0;                    // +0x10
    bool fDpos = false;              // +0x14 (VU flag 0x100)
    bool fDuv = false;               // +0x18 (VU flag 0x200)
    // runtime
    struct WR* pwr = nullptr;        // +0x0C
    ALO* palo = nullptr;             // +0x00: the globset's owner (pso)
    int iglob = -1;                  // its glob (+0x04)
    std::vector<glm::vec4> astate;   // per matrix, per strip vertex: [imat * cvtx + ivtx] (the batches' apvWarp)
    uint32_t nGen = 0;               // (native) bumped when astate changes
};

struct GLOB
{
    MESHK meshk = MESHK_Strips;     // hdr+3
    int8_t rp = 0;                  // hdr+0 (the RPL kind, 5 bits)
    int8_t cbone = 0;               // hdr+1
    uint8_t grfMesh = 0;            // hdr+2 (type 0): 1 lit/bake, 2, 4 warp block, 8, 0x10 double-sided
    float gZOrder = 0;              // hdr+4 (g * |g|, or the "none" value)
    float uFog = 0;                 // passed to CM_BuildFogRecip
    int8_t rtck = 0;                // 0/1/2: billboard modes
    int32_t n0 = 0;                 // group+0
    uint32_t grfGroup = 0;          // group+0xC (0x800 hidden, 0x400 gZOrder, 0x1000 matrix baked, ...)
    bool fHidden = false;
    int16_t oidOwner = -1;          // S16 id looked up through the owner's vtable slot 51
    // Draw record (absent when the glob's first byte is non-zero).
    bool fDraw = false;
    glm::vec3 posCenter{0.0f};
    float sRadius = 0;
    uint32_t u14 = 0;
    float sMRD = 0;
    // A group without a draw record (the level's static geometry) is culled and sorted by its sphere in the glob set's
    // sphere sets (0x167870 kind 0; the volume tree 0x1D5808 keys its render record by the camera-to-sphere distance)
    bool fSphere = false;
    glm::vec3 posSphere{0.0f};
    float sRadiusSphere = 0;
    SAA* psaa = nullptr;            // group+0x20 (type 0: S16 index into the glob's SAAs)
    std::vector<GLOBBONE> abone;
    int8_t nAlphaCtl = 0;           // type 0: the S8 per batch (last)
    // Type 0's view-angle alpha (0x16A000 SPECPARAMS, P_G 638): iSpec -1 none, 0 the camera's direction, 1 the direction
    // from posSpec to the camera, 2 from each vertex to the camera; gain = ag[0] + ag[1] x + ag[2] x^2 (x = that unit
    // direction . the S8 normal, n x 127; ag[1] / ag[2] already x 1/127, 1/16129), clamped to [agClamp[0], agClamp[1]],
    // scales each vertex's colour-A alpha (rounded)
    int8_t iSpec = -1;
    float agSpec[3] = {};
    float agClamp[2] = {};
    glm::vec3 posSpec{0.0f};

    // Instance data: template index (-1 for a template), own matrix.
    int iglobTemplate = -1;
    bool fMatrix = false;           // instance: dmat holds its matrix (owner x dmat when baked, 0x1000)
    glm::mat4 dmat{1.0f};

    // Type 0 geometry: strip vertices (in emission order) and triangle indices.
    std::vector<VTXGLOB> avtx;
    std::vector<uint32_t> aindex;
    std::vector<SUBGLOB> asubglob;
    std::shared_ptr<WRBG> pwrbg;        // the warp block (mesh flag 4)
    std::shared_ptr<IDXTAB> pidxtab;    // the colour sets this GLOB was coloured from
    // Type 1
    std::vector<SUBCEL> asubcel;
    // Types 2/3
    std::vector<SUBFX> asubfx;

    // Renderer cache (gl objects; owned by render.cpp).
    struct GLGLOB* pgl = nullptr;
    bool fHistoryWorld = false;   // CID 0x69: positions and live material alpha come from its history

    // FGlobLit's answer and the vertex array it was found for
    mutable const VTXGLOB* pvtxLitKey = nullptr;
    mutable size_t cvtxLitKey = 0;
    mutable bool fLitKey = false;
};

// Whether any of the glob's vertices is lit at run time (VTXGLOB::fUnbaked), kept per vertex array: the flags are set
// at load (a bake only turns 1 into 2) and the array is only ever replaced whole (ApplyLighting's template copy).
inline bool FGlobLit(const GLOB& glob)
{
    if (glob.pvtxLitKey != glob.avtx.data() || glob.cvtxLitKey != glob.avtx.size())
    {
        bool f = false;
        for (const VTXGLOB& v : glob.avtx)
            if (v.fUnbaked) { f = true; break; }
        glob.fLitKey = f;
        glob.pvtxLitKey = glob.avtx.data();
        glob.cvtxLitKey = glob.avtx.size();
    }
    return glob.fLitKey;
}

// A sphere of a sphere set (0x167870): kind -1 end, 0 a group's bounds, 1 a link to another set.
struct GLOBSPHERE
{
    int8_t kind = -1;
    glm::vec3 pos{0.0f};
    float sRadius = 0;
    float sMRD = 0;
    uint8_t mask = 0;
    int16_t iglob = -1;     // kind 0
    int16_t iset = -1;      // kind 1
};

struct GLOBSET
{
    ALO* palo = nullptr;
    bool fNoDraw = false;               // first byte: no draw records
    glm::vec3 posCenter{0.0f};
    float sRadius = 0;
    std::vector<SAA*> apsaa;            // +0x24/+0x28
    std::vector<int32_t> aids;          // +0x2C/+0x30
    std::vector<GLOB> aglob;            // +0x38 groups (templates and their instances)
    std::vector<std::array<GLOBSPHERE, 4>> asphereset;  // +0x48
    uint64_t grfglobset = 0;            // +0x50: bit 32 lit, bit 33 type-2 mesh
#ifdef __SWITCH__
    // (the Switch: per group whether it is a cel border, set at the load (glob.cpp), for the volume tree's walk
    // (render.cpp) instead of each group's large record)
    std::vector<uint8_t> afCelBorder;
#endif
};

// orig: 0x00167320 LoadGlob (entry point for the world loader: an ALO's mesh)
void LoadGlobsetFromBrx(GLOBSET* pglobset, ALO* palo, CBinaryInputStream* pbis);

// Statistics for the load test.
struct GLOBSTATS
{
    int cglobset = 0, cglob = 0, cinstance = 0, cvtx = 0, ctri = 0, cedge = 0, cfx = 0;
    int cmeshk[4] = {};
    int cskinned = 0, cfiltered = 0, cnoshd = 0;
    int cstrip = 0, cstripOdd = 0;
};
extern GLOBSTATS g_globstats;
