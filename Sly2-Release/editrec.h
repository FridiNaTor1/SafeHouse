#pragma once

// PC addition (the map editor, notes/EDITOR_2026-11-12.md): where the world loader read each object and its editable
// fields, as offsets in the world file (the parts put back together, worldparts.h). Null (always, but while the
// editor loads a world): nothing is recorded and the loaders behave exactly as before.

#include <cstddef>
#include <vector>

struct LO;

enum EDITFK : int
{
    EDITFK_LoxMatrix,    // a LOX's (or a proxy's) matrix: ReadMatrix4, 48 bytes (3 x 3 then the translation)
    EDITFK_XfMatrix,     // an ALO's transform record i's matrix (the records of the default kind)
    EDITFK_Option,       // an option's value (LoadOptionFromBrx, not the CRef arrays): eopid, its kind k
    EDITFK_Path,         // a PATH's curve (LoadPathFromBrx, cids 0x0A..0x0E): ib its S8 kind, i its point count, k the
                         // kind (0 linear, else cubic), eopid its S8 closed flag; then U16 c, c F32 parameters, c points
                         // (12 bytes each), cubic: c in- then c out-tangents
    EDITFK_OptionArray,  // a method / list option (grf 0x401: CRef values): eopid; ib after its S16 eopid
    EDITFK_TaskLinks,    // a TASK's links (LoadTaskFromBrx): ib its U8 prerequisite count (then S32 oids, U8 successor
                         // count, S32 oids, the name)
    EDITFK_Globset,      // an ALO's glob set (its model, LoadGlobsetFromBrx): [ib, ibEnd)
    EDITFK_Soes,         // an ALO's collision entries (S16 count, the SOEs, then mass + inertia when any): [ib, ibEnd)
};

struct EDITOBJREC
{
    LO* plo = nullptr;       // null: a proxy that made no object of its own, or a record the filter skipped
    int cid = 0, oid = 0;
    int depth = 0;           // 0: a group's top-level record (in its Objects part); more: inside another object's
    std::size_t ibRecord = 0;   // the record's first byte (its S8 filter count)
    std::size_t ibBody = 0;     // its data (after cb)
    std::size_t cb = 0;
    std::size_t ibMatrix = 0;   // a proxy's matrix (it places its group's instance; 0: none)
    std::size_t ibList = 0;     // the S16 count of the record list it is in (LoadSwObjsFromBrx)
};

struct EDITFIELD
{
    LO* plo = nullptr;
    EDITFK fk = EDITFK_LoxMatrix;
    int i = 0;          // EDITFK_XfMatrix: the record's index
    int eopid = -1;     // EDITFK_Option
    int k = -1;         // EDITFK_Option: the option's kind
    std::size_t ib = 0;
    std::size_t ibEnd = 0;   // EDITFK_Option / EDITFK_OptionArray: the first byte after the value(s)
};

struct EDITRECORDER
{
    std::vector<EDITOBJREC> aobj;
    std::vector<EDITFIELD> afield;
    int depth = 0;
    std::size_t ibProxyMatrix = 0;   // (LoadProxyFromBrx: its matrix, kept until it makes its object)
};

extern EDITRECORDER* g_peditrec;

// The option descriptors (brx.cpp, the executable's table): an option's kind and flags (false: no such eopid).
bool FEditorOptionInfo(int eopid, int& k, int& grf);
// An option's new value bytes (as ReadOptionValue reads them) applied to the live object the way the loader applies
// it, replacing its OPT entry. False (why says it) for the options that make or link objects at the load (methods,
// sub-objects, arrays, warps, booty): those show after a reload.
bool FEditorApplyOption(LO* plo, int eopid, const void* pv, std::size_t cb, const char** pwhy);
