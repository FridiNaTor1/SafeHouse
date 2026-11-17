#pragma once

// Loading objects and options from world files (Sly 1's brx.c): PloNew, LoadSwObjsFromBrx, the option loaders
// (EOPID table at 0x2AC880, read from the executable).

#include "lo.h"

#include <map>
#include <vector>

class CBinaryInputStream;
struct SW;

// The original's object sinks (an interface {vt, ...} whose slot 1 appends objects): the caller's list, or the
// loader's own one for objects of shared groups (k = -1 / -2), which also sets the objects' flags.
struct LOSINK
{
    std::vector<LO*>* papo = nullptr;
    bool fStack = false;  // the loader's own sink (0x13FA30)
    int n = -1;           // its +4: 1 for k = -2, 0 for k = -1
    int m = -1;           // its +8: the key (vt slot 25 on each object)
    void Append(LO* plo);
};

// The object whose load is running (null: the world loader's own level)
LO* PloLoading();
// orig 0x1F7B98: an ALO's child of class 0x12 oid 391, made when it has none (also the getter of options 49..51)
LO* PloEnsureChild391(ALO* palo);
// orig 0x13E3C8 PloNew
LO* PloNew(CID cid, SW* psw, ALO* paloParent, OID oid, int iframe);
// orig 0x13E510 PloCloneLo
LO* PloCloneLo(LO* plo, SW* psw, ALO* paloParent);
// orig 0x13FB40 LoadSwObjsFromBrx: psw's objects (paloParent: the object loading its children, or null)
void LoadSwObjsFromBrx(SW* psw, ALO* paloParent, CBinaryInputStream* pbis, LOSINK* psink);
// orig 0x13F810 LoadOptionsFromBrx: options until a negative id. plo: the object (for field options), or null
// (the SW / camera option blocks).
void LoadOptionsFromBrx(std::vector<OPT>& aopt, LO* plo, CBinaryInputStream* pbis);
// orig 0x13E818
void LoadOptionFromBrx(OPT& opt, LO* plo, int eopid, int grf, CBinaryInputStream* pbis);
// orig 0x13E690
OPTVAL RefReadOption(int k, CBinaryInputStream* pbis);
// PC: where a field option (EOPID grf 0x80 / 0x10) lives in LO::afield: (sub-object getter (0: the object), offset).
// One key per (class, option) for the loader, the splice's seto! and the code (brx.cpp "where field options live"):
// the Sly 2 twin's offset (the copied code's), else Sly 3's (FIELD_TAG3 apart where that would collide).
struct OPTFIELDKEY
{
    bool fValid = false;
    uint32_t pfnGet = 0;
    uint32_t ib = 0;
    bool operator==(const OPTFIELDKEY& o) const { return fValid == o.fValid && pfnGet == o.pfnGet && ib == o.ib; }
};
OPTFIELDKEY OptionFieldKey(int cid, int eopid3);
// The original's key (the record's +0x24 getter, +0x18 ibFile / +0x10 ibSet)
OPTFIELDKEY Sly3OptionFieldKey(int eopid3);
// The store of a word at the original's offset (the class's field option there, else the offset unchanged)
void FieldWordFromSly3(int cid, uint32_t pfnGet3, uint32_t ib3, uint32_t* ppfnGet, uint32_t* pib);
// The key the loader stores an option's value at (0x13E818's ibFile, +0x18; invalid when it stores none)
OPTFIELDKEY LoadFieldKey(int cid, int eopid3);
// The key a splice's seto! stores it at (0x201110's ibSet, +0x10; spliceglue.cpp; invalid when it stores none)
OPTFIELDKEY SetterFieldKey(int cid, int eopid3);
// (debug) --option-fields PATH: every class's field options with both keys (a TSV), the mismatches on stdout.
// SLY3_FIELDAUDIT=DIR: the native reads / writes of LO::afield (class, getter, offset, caller) written to
// DIR/reads.PID.tsv at exit; tools/optfields3.py joins them with the table.
int OptionFieldsCommand(const char* path);
// orig 0x132B10 FIsBasicDerivedFrom
void SetLoMatrix(LO* plo, const glm::mat4& m);  // vt slot 37 (loaders.cpp)
void SetLoChannel(LO* plo, XF* pxf);            // vt slot 10's channel part (loaders.cpp)
bool FIsLoDerivedFrom(const LO* plo, CID cid);

// The walk (--dump-world): the world loader reads every block as the original does, but makes no objects (their
// bodies are skipped by the size their header carries, as the original does for filtered objects) and does not
// evaluate the splice modules. What it saw, per world:
struct WALKOBJ
{
    std::size_t ib = 0;
    int igrp = -1, mode = 0, cid = 0, n = 0;
    OID oid = 0;
    int32_t cb = 0;
    std::vector<int16_t> afilter;
    std::vector<int32_t> acond;
};
struct WALK
{
    bool fOn = false;
    std::vector<WALKOBJ> aobj;
    std::map<int, int> mpcidc, mpmodec;
    int cmodule = 0, cpairSplice = 0;
    std::map<int, int> mptagc;
};
extern WALK g_walk;
void WalkSpliceFromBrx(SW* psw, int a, CBinaryInputStream* pbis);   // splicemap.cpp (parse only)

// Statistics of the last loads (for --load-world's report).
struct BRXSTATS
{
    int cobj = 0, cskipFilter = 0, cmission = 0, cskipShared = 0, cshared1 = 0, cshared2 = 0, cproxy = 0, ccbMismatch = 0;
    // (Sly 3 development, per Sly 3 class: made with no loader, only the ALO part read, the rest of the body skipped,
    // the read failed)
    std::map<int, int> mpcidcNoLoader, mpcidcAloOnly, mpcidcSkipped, mpcidcError;
};
extern BRXSTATS g_brxstats;
