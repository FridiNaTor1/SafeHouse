#pragma once

// Sly 3's object loading (the Sly 3 class loaders): the object stream of a group (orig3 0x151D80 LoadSwObjsFromBrx), PloNew
// (0x150690), PloCloneLo (0x1507E0), the option readers (0x150AE8 / 0x150960 / 0x1519C8 with the EOPID table
// 0x32CA80), the proxy loader (0x1FA778) and the classes' loaders (vt slot 14, keyed by their Sly 3 addresses in
// cls.cpp's registry). The objects are Sly2-Release's native structs (lo.h: LO / LOX / ALO ...) in Sly2-Release's SW
// (sw.h), with Sly 3 class ids (cls.h g_acls is the Sly 3 table).

#include "../brx.h"
#include "../lo.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class CBinaryInputStream;
struct SW;

// The world's camera (orig3: the static CM at 0x36BC10, class 0xAC): the option-set loader 0x1500F0 picks one of the
// world's option blocks, stores its id at CM +0x10 and loads the block's options into the camera; the object
// conditions (0x151D10), the BMP / glob conditions and the node sets test their ids against it (0x169150: CM +0x10,
// then the extra ids at CM +0xDC count / +0xE0, added by 0x1694D8). The record is condset3.h's (g_condset3, with the
// fog / colour fields the block's options set; g_nFilterCur3 is the world filter 0x47989C).
#include "condset3.h"
struct CMBLOCK3
{
    int iblock = -1;              // the option block chosen (0x1500F0's s1)
    std::vector<OPT> aoptCm;      // its options (loaded into the camera: for the Sly 3 player)
};
extern CMBLOCK3 g_cmblock3;
// The EOPID's +0x08 (the setter of grf 0x100, the method of 0x400)
uint32_t PfnEopid3(int eopid);
// The placed ALOs' record world matrices (XF::matWorld), parents first.
void UpdateLoWorlds3(SW* psw);

// The arrival keys of 0x1500F0 / 0x14FA38 (orig3: GAME 0x36C700 +0x1498 / +0x14A0): -1 at the boot (PCSX2 st1, st2);
// 1797 / 1798 after START (st3, st4: the world is not reloaded).
struct ARRIVAL3
{
    int32_t keyA = -1, keyB = -1;
};

// The loader's object sink (the original's {vt, n, key} interface whose slot 1 appends; 0x151D80 makes a stack one
// for the shared modes 2 / 3).
struct LOSINK3
{
    std::vector<LO*>* papo = nullptr;
    bool fStack = false;
    int n = -1;           // +4: 1 for mode 2, 0 for mode 3
    int32_t key = -1;     // +8: 0x1FA5A8's value
    void Append(LO* plo);
};

// orig3: 0x150960 RefReadOption, 0x150AE8 LoadOptionFromBrx, 0x1519C8 LoadOptionsFromBrx (plo: the object, or null
// for the world's / the camera's blocks). Returns the count of options read.
OPTVAL RefReadOption3(int k, CBinaryInputStream* pbis);
void LoadOptionFromBrx3(OPT& opt, LO* plo, int eopid, int grf, CBinaryInputStream* pbis);
int LoadOptionsFromBrx3(std::vector<OPT>& aopt, LO* plo, CBinaryInputStream* pbis);

// orig3: 0x150690 PloNew(cid, paloParent, oid, iframe) (the world is the global SW; here psw)
LO* PloNew3(SW* psw, CID cid, ALO* paloParent, OID oid, int iframe);
// orig3: 0x241268 (twin PloEnsureChild391): the ALO's 0x11 child oid 422, made when it has none
LO* PloEnsureChild422(ALO* palo);
// The objects the PostLoads (vt slot 17) make, after the world's objects are loaded (load3.cpp)
void PostLoadSwObjects3(SW* psw);
// orig3: 0x1507E0 PloCloneLo(plo, paloParent): PloNew of the class + vt slot 44 (CloneLo)
LO* PloCloneLo3(SW* psw, LO* plo, ALO* paloParent);
// orig3: 0x151D80 LoadSwObjsFromBrx(paloParent, pbis, psink)
void LoadSwObjsFromBrx3(SW* psw, ALO* paloParent, CBinaryInputStream* pbis, LOSINK3* psink);
// vt slot 42 (SetMatrix) / slot 10's channel part, for the Sly 3 classes
void SetLoMatrix3(LO* plo, const glm::mat4& m);
void SetLoChannel3(LO* plo, XF* pxf);
// The world being loaded (set by FWalkWorld3's load mode).
extern SW* g_psw3;

// Statistics and errors of the last load (--load-world's report)
struct LOAD3STATS
{
    int cobj = 0, cproxy = 0, cmission = 0, cfiltered = 0, cshared = 0, cskipShared = 0, cclone = 0;
    int ctopFail = 0, ctopOk = 0;              // top-level objects whose loader failed (skipped to their size) / read exactly cb
    std::map<std::string, int> mperrc;         // error -> count
    std::map<int, int> mpcidcFail;             // top-level class -> failures
    int cscptMisread = 0;                      // SCPT command lists the draft reader (scpt3.cpp) misread: skipped
    std::map<int, int> mpcidcUnported;         // objects of classes without a native loader (made, body skipped)
};
extern LOAD3STATS g_load3stats;
// The end of the body of the object whose loader runs (its record's size): for loaders whose reads are drafts
extern std::size_t g_ibBodyEnd3;

// The native structs of Sly 3 classes whose loaders keep data the Sly 2 structs (lo.h) have no place for.
#include "scpt3.h"
#include "screen3.h"
// (named LOSCPT3: scptrun3.h's SCPT3 is the executor's script record, made from this object)
struct LOSCPT3 : LO  // class 0x87 (orig3 0x2250C0: the LO's options, then the command list 0x225220, the Sly 3 splice port's)
{
    std::vector<SCPTCMD3> acmd;  // +0x40 count, +0x44 the 16-byte commands
};

// Class 0x2D (0x100 bytes; orig3 0x1FE270): Sly 3's particle effect ("pe_*", "psys_*", "xps_*", "explode_*" in
// the oid names), module 0x1FC000..0x203200 (no Sly 2 twin; render3.md "Particle effects"). +0xD8 the kind
// (0 explosion-like, 1 particle system, 2 emitter, 3 a camera-attached effect), +0xD0 / +0xD4 the components: 0x20-byte
// records made by 0x1FD758 (class 0xBF: a particle stream, run-time object vt 0x3615B8, drawn by 0x201390) or 0x1FDE70
// (class 0xC0: a RIP stream, run-time vt 0x361578, drawn through the RIPs 0x20C470), each loaded as an option list
// (0x1FD9D8 -> 0x1FD198 -> LoadOptionsFromBrx 0x1519C8) onto its records.
struct PECOMP3
{
    int cid = 0;                 // 0xBF / 0xC0
    int32_t oid = 0;             // +4
    int n = 0;                   // the item's S16 (+8)
    std::vector<OPT> aopt;       // its options (EOPIDs of the classes 0xBF / 0xC0)
    std::shared_ptr<LOXEMIT::EMITMESH> pmesh;  // the emission mesh (position kind 3: 0x1FCF10)
};
struct LOPE3 : LOX
{
    std::vector<PECOMP3> acomp;  // +0xD0 count, +0xD4 the records (the ones whose filters / conditions pass)
    int cskipped = 0;            // items filtered out (skipped by their size)
};
struct PE3STATS
{
    int cpe = 0, ccomp = 0, ccompBF = 0, ccompC0 = 0, cfiltered = 0, cbad = 0;  // cbad: payloads not read exactly
};
extern PE3STATS g_pe3stats;

// Class 0x97 (0x6A0 bytes; orig3 0x16EE98): LoadLoFromBrx, S32 n then n x {a file key (ReadCb64), S32, S32} (+0x40
// list), S32 m then m x {S8 kind: 1 S32 S32, 0 / 2 S32, else nothing} (+0x48 count), then 0x196C18 / InitZpr 0x196AA8.
struct LO97 : LO
{
    struct ENTRY { std::array<uint8_t, 64> abKey{}; int32_t a = 0, b = 0; };
    struct ITEM { int kind = 0; int32_t a = 0, b = 0; };
    std::vector<ENTRY> aentry;
    std::vector<ITEM> aitem;
};

// The screen objects 0x9E..0xA7 (orig3 0x260810, Sly 2 twin LoadScreenObjFromBrx 0x212298 at 0.70): LoadLoFromBrx, S8
// fExternal, F32 (+0x80), U32 (+0x84), U32 (+0x88), U32 (+0xAC); external (+0x48 = 0): a file key (ReadCb64 into a
// 0x50-byte block, AppendSlEntry 0x4797A8); else (+0x48 = 1) the elements 0x263118 (U16 count, per element U16 index
// then LoadElemFromBrx 0x263748: U32 kind, table 0x327F40 (9 kinds), building GIF packets) and the groups 0x2631C8.
// The body is read by screen3.cpp's LoadScreenObj3 (the Sly 3 renderer's reader of 0x260810 / 0x263118 / 0x2631C8,
// checked over all 1,671 screen objects of the 39 worlds): so holds the elements and the groups for the HUD /
// dialog drawing (the Sly 3 renderer); the fields below repeat its head.
struct LOSCREEN3 : LO
{
    int fExternal = 0;
    float g80 = 0.0f;
    uint32_t ul84 = 0, ul88 = 0, ulAC = 0;
    std::array<uint8_t, 64> abKey{};   // fExternal: the file's key record
    std::size_t ibElems = 0;           // !fExternal: where the elements start in the world file
    SCREENOBJ3 so;                     // screen3.h: the elements (by index) and the groups
};

// The object census of a loaded world (--load-world W --census): one line per object, as tools/census3.py reads
// it (the same as its PCSX2 RAM walk): "obj cid oid parentCid parentOid igrp clone".
void PrintObjectCensus3(SW* psw, FILE* pf);
// (debug) every option of an object as loaded (SLY3_OPTPRINT=oid,oid... with the game or --load-world)
void PrintObjectOptions3(const LO* plo, FILE* pf);
