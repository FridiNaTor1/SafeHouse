#pragma once

// Loading objects and options from world files (Sly 1's brx.c): PloNew, LoadSwObjsFromBrx, the option loaders
// (EOPID table at 0x2AC880, read from the executable).

#include "lo.h"

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
// (Sly 3) PloNew with the native struct given (pfnNew): objects the class table's struct does not fit (Sly 3's blends,
// class 0x83 = Sly 2's 0x6E, share the ASEG loader's struct in the table but are ASEGBL natively)
LO* PloNewNative(CID cid, SW* psw, ALO* paloParent, OID oid, int iframe, PFNNEW pfnNew);
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
// orig 0x132B10 FIsBasicDerivedFrom
void SetLoMatrix(LO* plo, const glm::mat4& m);  // vt slot 37 (loaders.cpp)
void SetLoChannel(LO* plo, XF* pxf);            // vt slot 10's channel part (loaders.cpp)
bool FIsLoDerivedFrom(const LO* plo, CID cid);

// Statistics of the last loads (for --load-world's report).
struct BRXSTATS
{
    int cobj = 0, cskipFilter = 0, cmission = 0, cskipShared = 0, cshared1 = 0, cshared2 = 0, cproxy = 0, ccbMismatch = 0;
};
extern BRXSTATS g_brxstats;
