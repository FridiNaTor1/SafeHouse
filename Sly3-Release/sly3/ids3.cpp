#include "ids3.h"

#include <cstdio>

namespace
{
const CLS3GEN s_acls3[] = {
#include "cls3gen.inc"
};
static_assert(sizeof(s_acls3) / sizeof(s_acls3[0]) == static_cast<int>(CID3::Max));

const EOPID3MAP s_aeopid3[] = {
#include "eopid3map.inc"
};
static_assert(sizeof(s_aeopid3) / sizeof(s_aeopid3[0]) == static_cast<int>(EOPID3::Max));

const BIF3MAP s_abif3[] = {
#include "bif3map.inc"
};
static_assert(sizeof(s_abif3) / sizeof(s_abif3[0]) == static_cast<int>(BIFK3::Max));

const CLS3GEN s_clsNone = {-1, -1, 0, 0, 0, 0, 0, -1, -1, ""};
const EOPID3MAP s_eopidNone = {-1, 0, 0, 0, -1, -1};
const BIF3MAP s_bifNone = {-1, 0, 0, 0, -1, -1};

// The names of the classes with a role (ids3.h CID3)
const char* SzNamed(int cid)
{
    switch (static_cast<CID3>(cid))
    {
    case CID3::SW: return "SW";
    case CID3::LO: return "LO";
    case CID3::LOX: return "LOX";
    case CID3::VOLUME: return "VOLUME";
    case CID3::PROXY: return "PROXY";
    case CID3::PATH: return "PATH";
    case CID3::ZONE: return "ZONE";
    case CID3::MESHZONE: return "MESHZONE";
    case CID3::SHAPE: return "SHAPE";
    case CID3::TRIGGER: return "TRIGGER";
    case CID3::EMITTER: return "EMITTER";
    case CID3::GROUPER: return "GROUPER";
    case CID3::EXPL: return "EXPL";
    case CID3::ALO: return "ALO";
    case CID3::SO: return "SO";
    case CID3::PO: return "PO";
    case CID3::PLAYER: return "PLAYER";
    case CID3::JT: return "JT";
    case CID3::MURRAY: return "MURRAY";
    case CID3::BENTLEY: return "BENTLEY";
    case CID3::GURU: return "GURU";
    case CID3::PANDA_KING: return "PANDA_KING";
    case CID3::PENELOPE: return "PENELOPE";
    case CID3::CARMELITA: return "CARMELITA";
    case CID3::GUARD: return "GUARD";
    case CID3::ASEG: return "ASEG";
    case CID3::SM: return "SM";
    case CID3::MISSION: return "MISSION";
    case CID3::TASK: return "TASK";
    case CID3::CM: return "CM";
    default: return nullptr;
    }
}
}  // namespace

const CLS3GEN& Cls3(int cid)
{
    return cid >= 0 && cid < static_cast<int>(CID3::Max) ? s_acls3[cid] : s_clsNone;
}

const char* SzCid3(int cid)
{
    if (const char* sz = SzNamed(cid)) return sz;
    static char s_ach[static_cast<int>(CID3::Max)][12];
    if (cid < 0 || cid >= static_cast<int>(CID3::Max)) return "CID3_?";
    if (!s_ach[cid][0]) std::snprintf(s_ach[cid], sizeof(s_ach[cid]), "CID3_%02X", cid);
    return s_ach[cid];
}

std::string SzCid3Twin(int cid)
{
    const CLS3GEN& c = Cls3(cid);
    if (c.cid2 < 0) return "";
    char sz[64];
    if (c.score >= 0) std::snprintf(sz, sizeof(sz), "sly2 0x%02X %s %.2f", c.cid2, c.name2, c.score / 100.0);
    else std::snprintf(sz, sizeof(sz), "sly2 0x%02X %s (option kinds)", c.cid2, c.name2);
    return sz;
}

bool FCid3DerivedFrom(int cid, CID3 cidBase)
{
    for (int c = cid, guard = 0; c >= 0 && guard < 64; c = Cls3(c).cidBase, ++guard)
        if (c == static_cast<int>(cidBase)) return true;
    return false;
}

int Cid2FromCid3(int cid) { return Cls3(cid).cid2; }

int Cid3FromCid2(int cid2)
{
    for (const CLS3GEN& c : s_acls3)
        if (c.cid2 == cid2 && cid2 >= 0) return c.cid;
    return -1;
}

const EOPID3MAP& Eopid3(int eopid)
{
    return eopid >= 0 && eopid < static_cast<int>(EOPID3::Max) ? s_aeopid3[eopid] : s_eopidNone;
}

int Eopid2FromEopid3(int eopid3) { return Eopid3(eopid3).eopid2; }

int Eopid3FromEopid2(int eopid2)
{
    int best = -1, score = -1;
    for (const EOPID3MAP& e : s_aeopid3)
        if (eopid2 >= 0 && e.eopid2 == eopid2 && e.score > score)
        {
            best = e.eopid3;
            score = e.score;
        }
    return best;
}

const BIF3MAP& Bif3(int bif) { return bif >= 0 && bif < static_cast<int>(BIFK3::Max) ? s_abif3[bif] : s_bifNone; }

int Bif2FromBif3(int bif3) { return Bif3(bif3).bif2; }

int Bif3FromBif2(int bif2)
{
    int best = -1, sim = -1;
    for (const BIF3MAP& b : s_abif3)
        if (bif2 >= 0 && b.bif2 == bif2 && b.sim > sim)
        {
            best = b.bif3;
            sim = b.sim;
        }
    return best;
}

int Tagk2FromTagk3(int tag3)
{
    if (tag3 >= static_cast<int>(TAGK3::S32) && tag3 <= static_cast<int>(TAGK3::Basic)) return tag3 + 2;
    if (tag3 >= 0 && tag3 <= static_cast<int>(TAGK3::DefineMacro)) return tag3;
    if (tag3 >= static_cast<int>(TAGK3::Assert) && tag3 <= static_cast<int>(TAGK3::Begin)) return tag3 + 1;
    if (tag3 >= static_cast<int>(TAGK3::Quote) && tag3 <= static_cast<int>(TAGK3::Dot)) return tag3 + 2;
    return -1;
}
