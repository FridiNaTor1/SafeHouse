// cmcheck.cpp: differential checks of the native camera (cm.cpp, cplcy.cpp) against the reference.
//
// Input: a trace of the reference running the original game (Sly2-Reference with SLY2_FNTRACE, see
// Sly2-Reference/Sly2-Release/runtime/interp.cpp): UpdateCm 0x14D478's entry / exit with the clock, the camera, the pads, the focus
// and the zones' RAM, and the helpers it calls (their registers and stack). Two modes:
//   --cm-check TRACE         each UpdateCm alone: the native camera is loaded from the entry's RAM, runs, and is
//                            compared with the exit's (a unit check of every frame);
//   --cm-check TRACE --cm-free   the native camera is loaded once, then runs on its own for the whole trace with the
//                            reference's inputs (clock, pad, focus, zones, the stack's pushes and pops): the camera's
//                            path is compared frame by frame.
// The world queries the camera makes (curves, collision avoidance, visibility) are not native yet: the reference's
// results are replayed from the trace (the arguments are compared too).
#include "actor.h"
#include "game/sly3.h"
#include "so.h"
#include "cm.h"
#include "cplcy.h"
#include "cls.h"
#include "lo.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace cmm;

namespace
{
constexpr uint32_t A_CLOCK = S3VA(0x2DDED0), A_CM = S3VA(0x2DDF40);
constexpr uint32_t PC_UPDATECM = 0x14D478, PC_SETCMPOLICY = 0x14E328, PC_REVOKECMPOLICY = 0x14E4C8;

struct REC
{
    uint32_t kind = 0, icall = 0, vb = 0, pc = 0;
    uint8_t agpr[32 * 16]{};
    uint32_t afpr[32]{};
    uint8_t abStack[0x400]{};
    std::vector<uint8_t> abRanges;
    uint32_t Gpr(int r) const { uint32_t u; std::memcpy(&u, agpr + 16 * r, 4); return u; }
    glm::vec4 GprVec(int r) const { glm::vec4 v; std::memcpy(&v, agpr + 16 * r, 16); return v; }
    float Fpr(int r) const { float f; std::memcpy(&f, &afpr[r], 4); return f; }
    uint32_t Sp() const { return Gpr(29); }
};

struct RANGE
{
    uint32_t a, cb, ib;  // address, size, offset in REC::abRanges
};

class TRACE
{
public:
    std::FILE* pf = nullptr;
    std::vector<RANGE> aranges;
    bool FOpen(const std::string& path, const std::string& ranges)
    {
        pf = std::fopen(path.c_str(), "rb");
        uint32_t ib = 0;
        for (size_t i = 0; i < ranges.size();)
        {
            size_t j = ranges.find(',', i);
            if (j == std::string::npos) j = ranges.size();
            unsigned a = 0, cb = 0;
            if (std::sscanf(ranges.substr(i, j - i).c_str(), "%x+%x", &a, &cb) == 2)
            {
                aranges.push_back({a, cb, ib});
                ib += cb;
            }
            i = j + 1;
        }
        return pf != nullptr;
    }
    bool FRead(REC& r)
    {
        uint32_t h[6];
        if (std::fread(h, sizeof h, 1, pf) != 1) return false;
        if (h[0] != 0x33544E46) return false;
        r.kind = h[1];
        r.icall = h[2];
        r.vb = h[3];
        r.pc = h[4];
        uint32_t cb = h[5];
        if (std::fread(r.agpr, sizeof r.agpr, 1, pf) != 1) return false;
        if (std::fread(r.afpr, sizeof r.afpr, 1, pf) != 1) return false;
        if (std::fread(r.abStack, sizeof r.abStack, 1, pf) != 1) return false;
        cb -= sizeof r.agpr + sizeof r.afpr + sizeof r.abStack;
        r.abRanges.resize(cb);
        if (cb && std::fread(r.abRanges.data(), cb, 1, pf) != 1) return false;
        return true;
    }
};

// The reference's memory at a record: its ranges, else the static RAM snapshot.
struct RAMVIEW
{
    const std::vector<RANGE>* paranges = nullptr;
    const REC* prec = nullptr;
    const std::vector<uint8_t>* pram = nullptr;
    bool FRead(uint32_t a, void* pv, uint32_t cb) const
    {
        if (prec && paranges)
            for (const RANGE& r : *paranges)
                if (a >= r.a && a + cb <= r.a + r.cb && r.ib + r.cb <= prec->abRanges.size())
                {
                    std::memcpy(pv, prec->abRanges.data() + r.ib + (a - r.a), cb);
                    return true;
                }
        if (pram && a + cb <= pram->size())
        {
            std::memcpy(pv, pram->data() + a, cb);
            return true;
        }
        std::memset(pv, 0, cb);
        return false;
    }
    uint32_t U32(uint32_t a) const { uint32_t u = 0; FRead(a, &u, 4); return u; }
    int32_t S32(uint32_t a) const { return static_cast<int32_t>(U32(a)); }
    float F32(uint32_t a) const { float f = 0; FRead(a, &f, 4); return f; }
    glm::vec4 V4(uint32_t a) const { glm::vec4 v(0.0f); FRead(a, &v, 16); return v; }
    glm::vec3 V3(uint32_t a) const { return glm::vec3(V4(a)); }
};

// LO tokens: the reference's object addresses as native pointers (never dereferenced by the camera).
LO* Tok(uint32_t a) { return a ? reinterpret_cast<LO*>(static_cast<uintptr_t>(a) + 0x100000000ull) : nullptr; }
uint32_t AddrOf(const LO* plo) { return plo ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(plo) - 0x100000000ull) : 0; }

struct CHECK
{
    TRACE tr;
    std::vector<uint8_t> ramStatic;
    RAMVIEW rvEntry, rvExit;
    REC recEntry, recExit;
    std::vector<REC> ahelper;   // the helper records of the current UpdateCm
    std::map<uint32_t, std::unique_ptr<ZONECM>> mpzone;  // by RAM address
    std::map<uint32_t, std::unique_ptr<CMOBJ>> mpcmobj;
    std::map<uint32_t, std::unique_ptr<CMTRANS>> mptrans;
    std::map<uint32_t, std::unique_ptr<CMSTATE>> mpstate;
    std::map<const void*, uint32_t> mpzoneAddr;
    size_t iCrvPos = 0, iEyeMove = 0, iEyeCut = 0, iCpdefi = 0;
    bool fFree = false;
    int cmismatchArg = 0;
} s_chk;

// ---- the reference's objects
uint32_t VtOf(uint32_t a, const RAMVIEW& rv) { return rv.U32(a + 0xC); }
int CidOfVt(uint32_t vt)
{
    for (int i = 0; i < CID_Max; ++i)
        if (g_acls[i].vtOrig == vt) return i;
    return -1;
}

// 0x1A6318 (vtable slot 31): an ALO's (0x1F3FA0) is its transform record's world matrix (*(+0x60) + 0x40), a LOX's
// (0x231440) its +0xA0.
uint32_t AMatOf(uint32_t a, const RAMVIEW& rv)
{
    const uint32_t vt = VtOf(a, rv);
    const int cid = CidOfVt(vt);
    if (cid >= 0 && FCidDerivedFrom(cid, S3CID(0x30))) return rv.U32(a + 0x60) + 0x40;
    return a + 0xA0;
}

const RAMVIEW& RvCur() { return s_chk.rvEntry; }

// (objects' world matrices from the exit's RAM: a LOX's +0xA0 is a cache 0x231368 fills when asked, so the entry's
// may be stale; objects do not move during UpdateCm)
glm::vec3 PosLoChk(LO* plo)
{
    const uint32_t m = AMatOf(AddrOf(plo), s_chk.rvExit);
    return s_chk.rvExit.V3(m + 0x30);
}
glm::mat3 MatLoChk(LO* plo)
{
    const uint32_t m = AMatOf(AddrOf(plo), s_chk.rvExit);
    glm::mat3 r;
    for (int i = 0; i < 3; ++i) r[i] = s_chk.rvExit.V3(m + 0x10 * i);
    return r;
}
bool FIsLoDerivedFromChk(LO* plo, int cid)
{
    const int c = CidOfVt(VtOf(AddrOf(plo), RvCur()));
    return c >= 0 && FCidDerivedFrom(c, cid);
}
CPDEFI CpdefiFromRam(const RAMVIEW& rv, uint32_t a)
{
    CPDEFI c;
    c.grfcpd = rv.U32(a);
    for (int i = 0; i < 3; ++i) c.aq0[i] = rv.U32(a + 4 + 4 * i);
    c.posBase = rv.V4(a + 0x10);
    c.vBase = rv.V4(a + 0x20);
    c.radHome = rv.F32(a + 0x30);
    c.sAdjust = rv.F32(a + 0x34);
    c.tMoveLast = rv.F32(a + 0x38);
    c.cfk = rv.S32(a + 0x3C);
    c.posAdjust = rv.V4(a + 0x40);
    return c;
}
const REC* PrecHelper(uint32_t pc, size_t& i, int kind);
const REC* PrecExitOf(const REC* pentry);
void GetSoCpdefiChk(LO* plo, CPDEFI* pcpdefi)
{
    (void)plo;
    // slot 50's result: written to CM +0x270 by UpdateCm (what the reference left there; its +0x40 is AdjustCmCpdefi's,
    // recomputed natively), or to the caller's stack (GetCmCpdefi in an activation)
    const REC* pe = PrecHelper(0x18A428, s_chk.iCpdefi, 0);
    if (pe && pe->Gpr(5) != A_CM + 0x270)
        if (const REC* px = PrecExitOf(pe))
        {
            const uint32_t ib = pe->Gpr(5) - px->Sp();
            if (ib + 0x50 <= sizeof px->abStack)
            {
                std::vector<uint8_t> ab(px->abStack + ib, px->abStack + ib + 0x50);
                RAMVIEW rv;
                std::vector<uint8_t> ram(0x50);
                std::memcpy(ram.data(), ab.data(), 0x50);
                rv.pram = &ram;
                *pcpdefi = CpdefiFromRam(rv, 0);
                return;
            }
        }
    *pcpdefi = CpdefiFromRam(s_chk.rvExit, A_CM + 0x270);
}
int JtfockChk(LO* plo) { return RvCur().S32(AddrOf(plo) + 0x4F4); }
const CMJOY* PjoyChk()
{
    static CMJOY s_joy;
    const RAMVIEW& rv = RvCur();
    const uint32_t a = (rv.U32(S3VA(0x2DFBC0)) & 1) ? S3VA(0x2DFC00) : S3VA(0x2E0D00);
    s_joy.x = rv.F32(a + 0x103C);
    s_joy.y = rv.F32(a + 0x1040);
    s_joy.uDeflect = rv.F32(a + 0x1044);
    s_joy.x2 = rv.F32(a + 0x1070);
    s_joy.y2 = rv.F32(a + 0x1074);
    s_joy.uDeflect2 = rv.F32(a + 0x1078);
    s_joy.grfbtn = rv.U32(a + 0x10B4);
    s_joy.grfbtnPressed = rv.U32(a + 0x10B8);
    return &s_joy;
}
float TChk() { return RvCur().F32(A_CLOCK); }
float DtChk() { return RvCur().F32(A_CLOCK + 4); }
float DtRealChk() { return RvCur().F32(A_CLOCK + 0x10); }
bool FStereoChk() { return RvCur().S32(0x3DD490 + 0x3E38) != 0; }
bool FieldLoChk(LO* plo, uint32_t ib, uint32_t* pu)
{
    *pu = RvCur().U32(AddrOf(plo) + ib);
    return true;
}

ZONECM* PzoneFromRam(uint32_t a, const RAMVIEW& rv, bool fRefresh);

bool FocusChk(LO* plo, CMFOCUS* pf)
{
    const RAMVIEW& rv = RvCur();
    const uint32_t a = AddrOf(plo);
    pf->fJt = FIsLoDerivedFromChk(plo, 0x34);
    pf->jtfock = rv.S32(a + 0x4F4);
    pf->posFocus = rv.V3(a + 0x500);
    pf->n4C8 = rv.S32(a + 0x4C8);
    pf->n4CC = rv.S32(a + 0x4CC);
    pf->n10E4 = rv.S32(a + 0x10E4);
    const uint32_t aZone = rv.U32(a + 0x528);
    pf->pvZoneCur = aZone ? PzoneFromRam(aZone, rv, false) : nullptr;
    pf->mat = MatLoChk(plo);
    return true;
}

// ---- replays of the helpers' results (in call order within the UpdateCm)
const REC* PrecHelper(uint32_t pc, size_t& i, int kind)
{
    for (; i < s_chk.ahelper.size(); ++i)
        if (s_chk.ahelper[i].pc == pc && s_chk.ahelper[i].kind == static_cast<uint32_t>(kind)) return &s_chk.ahelper[i++];
    return nullptr;
}
// the exit record matching an entry record (the next exit of the same pc and call index)
const REC* PrecExitOf(const REC* pentry)
{
    for (const REC& r : s_chk.ahelper)
        if (r.kind == 1 && r.pc == pentry->pc && r.icall == pentry->icall) return &r;
    return nullptr;
}
glm::vec3 V3Stack(const REC& r, uint32_t a)
{
    const uint32_t ib = a - r.Sp();
    glm::vec4 v(0.0f);
    if (ib + 16 <= sizeof r.abStack) std::memcpy(&v, r.abStack + ib, 16);
    return glm::vec3(v);
}

void CrvPosChk(LO* pcrv, float s, glm::vec3* ppos, glm::vec3* pdir)
{
    const REC* pe = PrecHelper(0x1E6F10, s_chk.iCrvPos, 0);
    if (!pe)
    {
        ++s_chk.cmismatchArg;
        *ppos = glm::vec3(0.0f);
        *pdir = glm::vec3(1, 0, 0);
        return;
    }
    if (pe->Gpr(4) != AddrOf(pcrv) || std::fabs(pe->Fpr(12) - s) > 0.05f + 1e-4f * std::fabs(s))
    {
        if (s_chk.cmismatchArg++ < 20)
            std::printf("  curve call: native %X s %.4f, reference %X s %.4f\n", AddrOf(pcrv), s, pe->Gpr(4), pe->Fpr(12));
    }
    const REC* px = PrecExitOf(pe);
    if (!px) return;
    *ppos = V3Stack(*px, pe->Gpr(5));
    *pdir = V3Stack(*px, pe->Gpr(6));
}
float CrvNearestChk(LO* pcrv, const glm::vec3& pos, float s)
{
    (void)pos; (void)s;
    // 0x1E7380 writes the zone's +0x140 (pgOut); its value at the exit
    for (auto& it : s_chk.mpzone)
        if (it.second->pcrv == pcrv) return s_chk.rvExit.F32(it.first + 0x140);
    return s;
}
bool EyeMoveChk(const glm::vec3& posFrom, const glm::vec3& posTo, glm::vec3* pposOut)
{
    const REC* pe = PrecHelper(0x14F110, s_chk.iEyeMove, 0);
    if (!pe) return false;
    const glm::vec3 a(pe->GprVec(5)), b(pe->GprVec(6));
    if (glm::length(a - posFrom) > 0.5f || glm::length(b - posTo) > 0.5f)
    {
        if (s_chk.cmismatchArg++ < 20)
            std::printf("  eye move: native (%.2f %.2f %.2f) -> (%.2f %.2f %.2f), reference (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)\n",
                        posFrom.x, posFrom.y, posFrom.z, posTo.x, posTo.y, posTo.z, a.x, a.y, a.z, b.x, b.y, b.z);
    }
    if (std::getenv("SLY2_CMDEBUGP"))
        std::printf("  eye move: native (%.4f %.4f %.4f) -> (%.4f %.4f %.4f), reference (%.4f %.4f %.4f) -> (%.4f %.4f %.4f)\n",
                    posFrom.x, posFrom.y, posFrom.z, posTo.x, posTo.y, posTo.z, a.x, a.y, a.z, b.x, b.y, b.z);
    const REC* px = PrecExitOf(pe);
    if (!px) return false;
    if (std::getenv("SLY2_CMDEBUGP"))
    {
        const glm::vec3 o(px->GprVec(2));
        std::printf("  eye move result (%.4f %.4f %.4f)\n", o.x, o.y, o.z);
    }
    // the original's result minus its input, applied to the native input (the avoidance's push)
    *pposOut = posTo + (glm::vec3(px->GprVec(2)) - b);
    return true;
}
bool EyeCutChk(void* pcplcy, const glm::vec3& posFocus, const glm::vec3& posEye, glm::vec3* pposOut, bool* pfBlocked)
{
    (void)pcplcy; (void)posFocus;
    const REC* pe = PrecHelper(0x223A70, s_chk.iEyeCut, 0);
    if (!pe) return false;
    const REC* px = PrecExitOf(pe);
    if (!px) return false;
    const glm::vec3 b(pe->GprVec(6));
    if (glm::length(b - posEye) > 0.5f && s_chk.cmismatchArg++ < 20)
        std::printf("  eye cut: native eye (%.2f %.2f %.2f), reference (%.2f %.2f %.2f)\n", posEye.x, posEye.y, posEye.z, b.x, b.y, b.z);
    // the reference's eye (a new point when the way was blocked; then the policy's mode became 4)
    *pposOut = V3Stack(*px, pe->Gpr(7));
    *pfBlocked = s_chk.rvExit.S32(A_CM + 0x3B0 + 0x34) == 4;
    return true;
}
bool VisibilityChk(void* pv)
{
    // 0x2236E8: the time the reference saw the focus (+0x80); a cut when it was lost for 2 s
    CPFOLLOW* p = static_cast<CPFOLLOW*>(pv);
    p->tVisible = s_chk.rvExit.F32(A_CM + 0x3B0 + 0x80);
    if (2.0f < TCm() - p->tVisible) CutCm(&g_cm);
    return true;
}

// ---- loading the native camera from the reference's RAM
ZONECM* PzoneFromRam(uint32_t a, const RAMVIEW& rv, bool fRefresh)
{
    auto& p = s_chk.mpzone[a];
    const bool fNew = !p;
    if (fNew) p = std::make_unique<ZONECM>();
    s_chk.mpzoneAddr[p.get()] = a;
    if (!fNew && !fRefresh) return p.get();
    ZONECM& z = *p;
    z.pzone = Tok(a);
    const uint32_t aCrv = rv.U32(a + 0x10C);
    z.pcrv = Tok(aCrv);
    z.crvk = aCrv ? rv.S32(aCrv + 0xE0) : 0;
    z.ccrvPoint = aCrv ? rv.S32(aCrv + 0xE8) : 0;
    const int cme = rv.S32(a + 0x110);
    const uint32_t ae = rv.U32(a + 0x114);
    z.acme.resize(cme > 0 && cme < 16 ? cme : 0);
    for (int i = 0; i < static_cast<int>(z.acme.size()); ++i)
    {
        const uint32_t e = ae + 0x90 * i;
        ZONECME& m = z.acme[i];
        m.cid = rv.S32(e);
        m.dtPath = rv.F32(e + 4);
        for (int k = 0; k < 3; ++k)
        {
            m.acoef[k] = rv.V4(e + 0x10 + 0x10 * k);
            m.alm[k] = LM{rv.F32(e + 0x40 + 8 * k), rv.F32(e + 0x44 + 8 * k)};
        }
        m.dxScreen = rv.F32(e + 0x58);
        m.dyScreen = rv.F32(e + 0x5C);
        m.uFocus = rv.F32(e + 0x60);
        m.uLead = rv.F32(e + 0x64);
        m.uSampleRate = rv.F32(e + 0x68);
        m.vecCoefY = rv.V3(e + 0x70);
        m.rSpring = rv.F32(e + 0x80);
        m.rDamping = rv.F32(e + 0x84);
        m.lmTilt = LM{rv.F32(e + 0x88), rv.F32(e + 0x8C)};
    }
    z.fPolar = rv.S32(a + 0x120);
    z.fArc = rv.S32(a + 0x12C);
    z.radPolarLimit = rv.F32(a + 0x130);
    z.tArc = rv.F32(a + 0x138);
    z.sPath = rv.F32(a + 0x140);
    z.posArc = rv.V3(a + 0x150);
    z.sArcRadius = rv.F32(a + 0x160);
    z.grf = rv.S32(a + 0x164);
    z.fReturn1 = rv.S32(a + 0x168);
    z.fReturn2 = rv.S32(a + 0x16C);
    z.dtReturn1 = rv.F32(a + 0x170);
    z.uReturn1 = rv.F32(a + 0x174);
    z.dtReturn2 = rv.F32(a + 0x178);
    z.uReturn2 = rv.F32(a + 0x17C);
    z.cfk = rv.S32(a + 0x180);
    z.fCutOnActivate = rv.S32(a + 0x184);
    z.radFOV = rv.F32(a + 0x188);
    z.fNoAvoid = rv.S32(a + 0x18C);
    z.ploLook = Tok(rv.U32(a + 0x190));
    for (int k = 0; k < 3; ++k) z.alead[k] = rv.F32(a + 0x1A0 + 4 * k);
    z.lmLead = LM{rv.F32(a + 0x1B0), rv.F32(a + 0x1B4)};
    return p.get();
}

CMOBJ* PcmobjFromRam(uint32_t a, const RAMVIEW& rv)
{
    auto& p = s_chk.mpcmobj[a];
    if (!p) p = std::make_unique<CMOBJ>();
    p->plo = Tok(a);
    p->ploTarget = Tok(rv.U32(a + 0xE0));
    p->fCut = rv.S32(a + 0x120);
    p->sFocal = rv.F32(a + 0x128);
    p->cFocal = rv.S32(a + 0x12C);
    p->radFOV = rv.F32(a + 0x130);
    return p.get();
}

CMSTATE* PstateFromRam(uint32_t a, const RAMVIEW& rv)
{
    auto& p = s_chk.mpstate[a];
    if (!p) p = std::make_unique<CMSTATE>();
    p->pos = rv.V3(a);
    p->eul = rv.V3(a + 0x10);
    p->radFOV = rv.F32(a + 0x20);
    return p.get();
}

CMTRANS* PtransFromRam(uint32_t a, const RAMVIEW& rv)
{
    auto& p = s_chk.mptrans[a];
    if (!p) p = std::make_unique<CMTRANS>();
    p->pstateA = PstateFromRam(rv.U32(a), rv);
    p->pstateB = PstateFromRam(rv.U32(a + 4), rv);
    p->dt = rv.F32(a + 8);
    p->fCut = rv.S32(a + 0xC);
    p->icurve = rv.S32(a + 0x10);
    return p.get();
}

bool AttachChk(LO* plo, CMATTACH* pattach)
{
    const RAMVIEW& rv = RvCur();
    const uint32_t a = AddrOf(plo);
    const uint32_t m = AMatOf(rv.U32(a + 0x1D0), s_chk.rvExit);
    for (int i = 0; i < 4; ++i) pattach->matMount[i] = s_chk.rvExit.V4(m + 0x10 * i);
    pattach->fSmooth = rv.S32(a + 0x1D4) != 0;
    pattach->radFOV = rv.F32(a + 0x1BC);
    return true;
}

void* PvFromRam(CPLCY* pcplcy, uint32_t a, const RAMVIEW& rv, bool fRefresh)
{
    if (!a) return nullptr;
    if (!pcplcy)
    {
        // (a revoke by the argument only: whatever native object stands for that address)
        if (auto it = s_chk.mpzone.find(a); it != s_chk.mpzone.end()) return it->second.get();
        if (auto it = s_chk.mpcmobj.find(a); it != s_chk.mpcmobj.end()) return it->second.get();
        if (auto it = s_chk.mptrans.find(a); it != s_chk.mptrans.end()) return it->second.get();
    }
    if (pcplcy == &g_cmpolicies.cpfollow) return PzoneFromRam(a, rv, fRefresh);
    if (pcplcy == &g_cmpolicies.cplookat) return PcmobjFromRam(a, rv);
    if (pcplcy == &g_cmpolicies.cptimed) return PtransFromRam(a, rv);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(a) + 0x100000000ull);
}

void LoadPolicyBase(CPLCY* p, uint32_t a, const RAMVIEW& rv)
{
    p->psoFocus = Tok(rv.U32(a));
    p->radFOVTargetSave = rv.F32(a + 4);
    p->sNearClipSave = rv.F32(a + 8);
}

// the follow policy's own state (CM +0x3B0: +0x18..+0x90), not its zone / entry
void LoadFollowFromRam(const RAMVIEW& rv)
{
    CPFOLLOW& f = g_cmpolicies.cpfollow;
    const uint32_t a = A_CM + 0x3B0;
    f.radPan = rv.F32(a + 0x18);
    f.radTilt = rv.F32(a + 0x1C);
    f.sDist = rv.F32(a + 0x20);
    f.dyScreen = rv.F32(a + 0x24);
    f.fManual = rv.S32(a + 0x28);
    f.radPanReturn = rv.F32(a + 0x2C);
    f.radTiltReturn = rv.F32(a + 0x30);
    f.cpmode = rv.S32(a + 0x34);
    f.fFirst = rv.S32(a + 0x38);
    f.fLeftSamples = rv.S32(a + 0x3C);
    f.dradPan = rv.F32(a + 0x40);
    f.dradTilt = rv.F32(a + 0x44);
    f.tManual = rv.F32(a + 0x48);
    f.radPolar = rv.F32(a + 0x4C);
    f.radPan1 = rv.F32(a + 0x50);
    f.radTilt1 = rv.F32(a + 0x54);
    f.radPan2 = rv.F32(a + 0x58);
    f.radTilt2 = rv.F32(a + 0x5C);
    f.tMoveLast = rv.F32(a + 0x60);
    f.uLead = rv.F32(a + 0x64);
    f.dsDist = rv.F32(a + 0x68);
    f.ddyScreen = rv.F32(a + 0x6C);
    f.posEye = rv.V4(a + 0x70);
    f.tVisible = rv.F32(a + 0x80);
    f.dradPanOffset = rv.F32(a + 0x84);
    f.dradTiltOffset = rv.F32(a + 0x88);
    f.ddradPanOffset = rv.F32(a + 0x8C);
    f.ddradTiltOffset = rv.F32(a + 0x90);
}

void LoadCmFromRam(const RAMVIEW& rv, bool fPolicies)
{
    CM& cm = g_cm;
    const uint32_t c = A_CM;
    cm.ploCur = Tok(rv.U32(c + 0x10));
    cm.nCur = rv.S32(c + 0x14);
    cm.rMRD = rv.F32(c + 0x18);
    cm.rMRDAdjust = rv.F32(c + 0x1C);
    cm.radFOV = rv.F32(c + 0x20);
    cm.radFOVTarget = rv.F32(c + 0x24);
    cm.dradFOV = rv.F32(c + 0x28);
    cm.smpaRadFOV = SMPA{rv.F32(c + 0x2C), rv.F32(c + 0x30), rv.F32(c + 0x34), rv.F32(c + 0x38)};
    cm.rAspect = rv.F32(c + 0x3C);
    cm.xScreenRange = rv.F32(c + 0x40);
    cm.yScreenRange = rv.F32(c + 0x44);
    cm.sRadiusNearClip = rv.F32(c + 0x48);
    cm.sNearClip = rv.F32(c + 0x4C);
    cm.sFarClip = rv.F32(c + 0x50);
    for (int i = 0; i < 3; ++i) cm.mat[i] = rv.V3(c + 0x60 + 0x10 * i);
    cm.pos = rv.V3(c + 0x90);
    for (int i = 0; i < 4; ++i)
    {
        cm.matProj.r[i] = rv.V4(c + 0x100 + 0x10 * i);
        cm.matWorldToClip.r[i] = rv.V4(c + 0x140 + 0x10 * i);
        cm.matClipToWorld.r[i] = rv.V4(c + 0x180 + 0x10 * i);
    }
    cm.sNearFog = rv.F32(c + 0x240);
    cm.sFarFog = rv.F32(c + 0x244);
    cm.uFogMax = rv.F32(c + 0x248);
    cm.rgbaFog = rv.U32(c + 0x24C);
    for (int i = 0; i < 4; ++i) cm.afFogRecip[i] = rv.F32(c + 0x250 + 4 * i);
    cm.psoFocusPrev = Tok(rv.U32(c + 0x260));
    cm.cpdefiPrev = CpdefiFromRam(rv, c + 0x270);
    cm.dposAdjust = rv.V4(c + 0x2C0);
    cm.vAdjust = rv.V4(c + 0x2D0);
    cm.dposFocus = rv.V4(c + 0x2E0);
    cm.vFocus = rv.V4(c + 0x2F0);
    cm.fCut = rv.S32(c + 0x300);
    cm.radCutPan = rv.F32(c + 0x304);
    cm.radCutTilt = rv.F32(c + 0x308);
    cm.tActivateCplcy = rv.F32(c + 0x390);
    cm.cFreeStarts = rv.S32(c + 0x4A4);
    cm.tJolt = rv.F32(c + 0x618);
    cm.nJolt = rv.S32(c + 0x61C);
    cm.sStereo = rv.F32(c + 0x620);
    cm.sStereoTarget = rv.F32(c + 0x624);
    if (!fPolicies) return;
    cm.ccpr = rv.S32(c + 0x30C);
    for (int i = 0; i < 8; ++i)
    {
        const uint32_t e = c + 0x310 + 0x10 * i;
        CPR& cpr = cm.acpr[i];
        cpr.cpp = rv.S32(e);
        const uint32_t aPol = rv.U32(e + 4);
        cpr.pcplcy = aPol >= c && aPol < c + 0x630 ? PcplcyFromCmOffset(aPol - c) : nullptr;
        cpr.psoFocus = Tok(rv.U32(e + 8));
        cpr.pv = i < cm.ccpr && cpr.pcplcy ? PvFromRam(cpr.pcplcy, rv.U32(e + 0xC), rv, true) : nullptr;
    }
    // the policies' states
    for (uint32_t ib : {0x3A0u, 0x3B0u, 0x450u, 0x4A8u, 0x4C0u, 0x500u, 0x520u, 0x5F0u, 0x600u})
        LoadPolicyBase(PcplcyFromCmOffset(ib), c + ib, rv);
    CPFOLLOW& f = g_cmpolicies.cpfollow;
    const uint32_t a = c + 0x3B0;
    const uint32_t aZone = rv.U32(a + 0x10);
    f.pzone = aZone ? PzoneFromRam(aZone, rv, false) : nullptr;
    const uint32_t aCme = rv.U32(a + 0x14);
    f.pcme = PcmeDefault();
    if (f.pzone && aCme)
    {
        const uint32_t ae = rv.U32(aZone + 0x114);
        const uint32_t i = (aCme - ae) / 0x90;
        if (aCme >= ae && i < f.pzone->acme.size()) f.pcme = &f.pzone->acme[i];
    }
    LoadFollowFromRam(rv);
    {
        CPFREE& q = g_cmpolicies.cpfree;
        const uint32_t b = c + 0x520;
        q.ploFocus = Tok(rv.U32(b + 0x10));
        q.ploTarget = Tok(rv.U32(b + 0x14));
        q.uZoomTarget = rv.F32(b + 0x18);
        q.posFocus = rv.V4(b + 0x20);
        q.posEye = rv.V4(b + 0x30);
        q.fScripted = rv.S32(b + 0x40);
        q.posScript = rv.V4(b + 0x50);
        q.eulScript = rv.V4(b + 0x60);
        q.radFOVScript = rv.F32(b + 0x70);
        q.posScriptTarget = rv.V4(b + 0x80);
        q.eulScriptTarget = rv.V4(b + 0x90);
        q.radFOVScriptTarget = rv.F32(b + 0xA0);
        q.radPan = rv.F32(b + 0xA4);
        q.radTilt = rv.F32(b + 0xA8);
        q.uZoom = rv.F32(b + 0xAC);
        q.duZoom = rv.F32(b + 0xB0);
        q.sZoom = rv.F32(b + 0xB4);
        q.radFOVNear = rv.F32(b + 0xB8);
        q.radFOVFar = rv.F32(b + 0xBC);
    }
    {
        CPRECON& q = g_cmpolicies.cprecon;
        const uint32_t b = c + 0x450;
        q.radPan = rv.F32(b + 0x10);
        q.radTilt = rv.F32(b + 0x14);
        q.dradPan = rv.F32(b + 0x18);
        q.dradTilt = rv.F32(b + 0x1C);
        q.uZoom = rv.F32(b + 0x20);
        q.duZoom = rv.F32(b + 0x24);
        q.sZoom = rv.F32(b + 0x28);
        q.radFOVNear = rv.F32(b + 0x2C);
        q.radFOVFar = rv.F32(b + 0x30);
        q.lmPan = LM{rv.F32(b + 0x3C), rv.F32(b + 0x40)};
        q.lmTilt = LM{rv.F32(b + 0x44), rv.F32(b + 0x48)};
        q.radPanStart = rv.F32(b + 0x4C);
        q.radTiltStart = rv.F32(b + 0x50);
        q.cStart = rv.S32(b + 0x54);
    }
    g_cmpolicies.cppair.ploA = Tok(rv.U32(c + 0x600 + 0x10));
    g_cmpolicies.cppair.ploB = Tok(rv.U32(c + 0x600 + 0x14));
    {
        const uint32_t aTrans = rv.U32(c + 0x500 + 0x10);
        g_cmpolicies.cptimed.ptrans = aTrans ? PtransFromRam(aTrans, rv) : nullptr;
    }
    CPLOOKAT& l = g_cmpolicies.cplookat;
    const uint32_t aObj = rv.U32(c + 0x4A8 + 0x10);
    l.pcmobj = aObj ? PcmobjFromRam(aObj, rv) : nullptr;
}

// ---- the comparison
struct STAT
{
    std::map<uint32_t, std::pair<int, int>> mpTop;  // the top policy's CM offset -> (frames, within)
    int c = 0, cOk = 0;
    double dposMax = 0, dmatMax = 0, dfovMax = 0, dclipMax = 0, dpolMax = 0;
    int vbWorst = -1;
};

float DmatClip(const MATRIX4& a, const MATRIX4& b)
{
    // (relative to each row's size: the translation row is in world units)
    float d = 0;
    for (int i = 0; i < 4; ++i)
    {
        const float s = std::max(1.0f, glm::length(b.r[i]));
        for (int k = 0; k < 4; ++k) d = std::max(d, std::fabs(a.r[i][k] - b.r[i][k]) / s);
    }
    return d;
}

void Compare(STAT& st, const RAMVIEW& rv, bool fPrint)
{
    const CM& cm = g_cm;
    const glm::vec3 pos = rv.V3(A_CM + 0x90);
    float dmat = 0;
    for (int i = 0; i < 3; ++i) dmat = std::max(dmat, glm::length(cm.mat[i] - rv.V3(A_CM + 0x60 + 0x10 * i)));
    const float dpos = glm::length(cm.pos - pos);
    const float dfov = std::fabs(cm.radFOV - rv.F32(A_CM + 0x20));
    MATRIX4 clip;
    for (int i = 0; i < 4; ++i) clip.r[i] = rv.V4(A_CM + 0x140 + 0x10 * i);
    const float dclip = DmatClip(cm.matWorldToClip, clip);
    const CPFOLLOW& f = g_cmpolicies.cpfollow;
    const uint32_t a = A_CM + 0x3B0;
    float dpol = 0;
    if (cm.ccpr > 0 && cm.acpr[0].pcplcy == &f)
    {
        dpol = std::max(dpol, std::fabs(RadNormalize(f.radPan - rv.F32(a + 0x18))));
        dpol = std::max(dpol, std::fabs(RadNormalize(f.radTilt - rv.F32(a + 0x1C))));
        dpol = std::max(dpol, std::fabs(f.sDist - rv.F32(a + 0x20)) / 100.0f);
    }
    st.c++;
    const bool fOk = dpos < 0.5f && dmat < 1e-3f && dfov < 1e-4f;
    if (fOk) st.cOk++;
    {
        const uint32_t ibTop = cm.ccpr > 0 ? CmOffsetFromPcplcy(cm.acpr[0].pcplcy) : 0;
        auto& e = st.mpTop[ibTop];
        e.first++;
        e.second += fOk;
    }
    if (dpos > st.dposMax) { st.dposMax = dpos; st.vbWorst = static_cast<int>(s_chk.recEntry.vb); }
    st.dmatMax = std::max<double>(st.dmatMax, dmat);
    st.dfovMax = std::max<double>(st.dfovMax, dfov);
    st.dclipMax = std::max<double>(st.dclipMax, dclip);
    st.dpolMax = std::max<double>(st.dpolMax, dpol);
    static const int s_vbDebug = std::getenv("SLY2_CMDEBUG") ? std::atoi(std::getenv("SLY2_CMDEBUG")) : -1;
    if (static_cast<int>(s_chk.recEntry.vb) == s_vbDebug)
    {
        const float an[] = {f.radPan, f.radTilt, f.sDist, f.dyScreen, (float)f.fManual, f.radPanReturn, f.radTiltReturn,
                            (float)f.cpmode, (float)f.fFirst, (float)f.fLeftSamples, f.dradPan, f.dradTilt, f.tManual, f.radPolar,
                            f.radPan1, f.radTilt1, f.radPan2, f.radTilt2, f.tMoveLast, f.uLead, f.dsDist, f.ddyScreen,
                            f.posEye.x, f.posEye.y, f.posEye.z, f.posEye.w, f.tVisible, f.dradPanOffset, f.dradTiltOffset,
                            f.ddradPanOffset, f.ddradTiltOffset};
        const uint32_t aib[] = {0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54,
                                0x58, 0x5C, 0x60, 0x64, 0x68, 0x6C, 0x70, 0x74, 0x78, 0x7C, 0x80, 0x84, 0x88, 0x8C, 0x90};
        for (size_t i = 0; i < sizeof aib / 4; ++i)
        {
            const uint32_t ib = aib[i];
            const bool fInt = ib == 0x28 || ib == 0x34 || ib == 0x38 || ib == 0x3C;
            std::printf("  +%02X native %12.6f ref %12.6f (entry %12.6f)\n", ib, an[i],
                        fInt ? (float)rv.S32(a + ib) : rv.F32(a + ib), fInt ? (float)s_chk.rvEntry.S32(a + ib) : s_chk.rvEntry.F32(a + ib));
        }
        std::printf("  cm dposFocus native %f %f %f ref %f %f %f; dposAdjust native %f %f ref %f %f\n", cm.dposFocus.x,
                    cm.dposFocus.y, cm.dposFocus.z, rv.F32(A_CM + 0x2E0), rv.F32(A_CM + 0x2E4), rv.F32(A_CM + 0x2E8),
                    cm.dposAdjust.x, cm.dposAdjust.y, rv.F32(A_CM + 0x2C0), rv.F32(A_CM + 0x2C4));
    }
    if (fPrint || (!fOk && st.c - st.cOk <= 10))
        std::printf("vb %5u %s pos native %9.2f %9.2f %8.2f ref %9.2f %9.2f %8.2f |d| %.3f  dmat %.2e dfov %.2e dclip %.2e dpol %.2e "
                    "mode %d/%d\n",
                    s_chk.recEntry.vb, fOk ? "ok " : "BAD", cm.pos.x, cm.pos.y, cm.pos.z, pos.x, pos.y, pos.z, dpos, dmat, dfov,
                    dclip, dpol, f.cpmode, rv.S32(a + 0x34));
}

// The native stack follows the reference's (the pushes and pops happen outside UpdateCm).
int SyncStack(const RAMVIEW& rv)
{
    int c0 = 0;
    CM& cm = g_cm;
    struct E { int cpp; CPLCY* p; LO* pso; uint32_t aPv; };
    std::vector<E> aref;
    const int c = rv.S32(A_CM + 0x30C);
    for (int i = 0; i < c && i < 8; ++i)
    {
        const uint32_t e = A_CM + 0x310 + 0x10 * i;
        const uint32_t aPol = rv.U32(e + 4);
        aref.push_back({rv.S32(e), PcplcyFromCmOffset(aPol - A_CM), Tok(rv.U32(e + 8)), rv.U32(e + 0xC)});
    }
    auto pvOf = [&](CPLCY* p, uint32_t a) { return PvFromRam(p, a, rv, false); };
    // pops: native entries not in the reference
    for (int i = 0; i < cm.ccpr;)
    {
        const CPR& cpr = cm.acpr[i];
        bool fFound = false;
        for (const E& e : aref)
            if (e.cpp == cpr.cpp && e.p == cpr.pcplcy && e.pso == cpr.psoFocus && pvOf(e.p, e.aPv) == cpr.pv) fFound = true;
        if (fFound) { ++i; continue; }
        RevokeCmPolicy(&cm, 15, cpr.cpp, cpr.pcplcy, cpr.psoFocus, cpr.pv);
        ++c0;
        i = 0;
    }
    // pushes (in the reference's order, lowest priority first)
    for (int k = static_cast<int>(aref.size()) - 1; k >= 0; --k)
    {
        const E& e = aref[k];
        bool fFound = false;
        for (int i = 0; i < cm.ccpr; ++i)
            if (e.cpp == cm.acpr[i].cpp && e.p == cm.acpr[i].pcplcy && e.pso == cm.acpr[i].psoFocus && pvOf(e.p, e.aPv) == cm.acpr[i].pv)
                fFound = true;
        if (!fFound && e.p)
        {
            SetCmPolicy(&cm, e.cpp, e.p, e.pso, pvOf(e.p, e.aPv));
            ++c0;
        }
    }
    return c0;
}

void BindWorld()
{
    g_cmworld = CMWORLD{};
    g_cmworld.pfnT = TChk;
    g_cmworld.pfnDt = DtChk;
    g_cmworld.pfnDtReal = DtRealChk;
    g_cmworld.pfnPosLo = PosLoChk;
    g_cmworld.pfnMatLo = MatLoChk;
    g_cmworld.pfnFIsLoDerivedFrom = FIsLoDerivedFromChk;
    g_cmworld.pfnGetSoCpdefi = GetSoCpdefiChk;
    g_cmworld.pfnJtfock = JtfockChk;
    g_cmworld.pfnPjoy = PjoyChk;
    g_cmworld.pfnFStereo = FStereoChk;
    g_cmworld.pfnFieldLo = FieldLoChk;
    g_cmfollowworld = CMFOLLOWWORLD{};
    g_cmfollowworld.pfnFocus = FocusChk;
    g_cmfollowworld.pfnCrvPos = CrvPosChk;
    g_cmfollowworld.pfnCrvNearest = CrvNearestChk;
    g_cmfollowworld.pfnCrvNearestAll = CrvNearestChk;
    g_cmfollowworld.pfnVisibility = VisibilityChk;
    g_cmfollowworld.pfnEyeCut = EyeCutChk;
    g_cmfollowworld.pfnEyeMove = EyeMoveChk;
    g_cmfollowworld.pfnAttach = AttachChk;
}
}  // namespace

int CmCheck(const std::string& pathTrace, const std::string& pathRam, const std::string& ranges, bool fFree, bool fVerbose)
{
    CHECK& k = s_chk;
    k.fFree = fFree;
    if (!k.tr.FOpen(pathTrace, ranges))
    {
        std::fprintf(stderr, "cannot open %s\n", pathTrace.c_str());
        return 2;
    }
    if (!pathRam.empty())
    {
        if (std::FILE* pf = std::fopen(pathRam.c_str(), "rb"))
        {
            k.ramStatic.resize(32u << 20);
            k.ramStatic.resize(std::fread(k.ramStatic.data(), 1, k.ramStatic.size(), pf));
            std::fclose(pf);
        }
    }
    for (RAMVIEW* prv : {&k.rvEntry, &k.rvExit})
    {
        prv->paranges = &k.tr.aranges;
        prv->pram = &k.ramStatic;
    }
    k.rvEntry.prec = &k.recEntry;
    k.rvExit.prec = &k.recExit;
    StartupCm();
    BindWorld();
    if (const char* pszWorld = std::getenv("SLY2_CMNATIVECRV"))
    {
        // the zones' curves natively (crv.cpp) instead of the trace's replays
        void BindNativeCurves();
        bool FLoadCrvWorldForCheck(const std::string& world);
        if (FLoadCrvWorldForCheck(pszWorld)) BindNativeCurves();
    }
    STAT st;
    bool fLoaded = false;
    REC r;
    int depth = 0;
    uint32_t pcRoot = 0, icallRoot = 0;
    int csync = 0, cpush = 0, cpop = 0;
    while (k.tr.FRead(r))
    {
        // the roots: UpdateCm, the stack's pushes / pops, the free camera's setters (scripts call them between frames)
        const bool fRoot = r.pc == PC_UPDATECM || r.pc == PC_SETCMPOLICY || r.pc == PC_REVOKECMPOLICY || r.pc == 0x150EA0 ||
                           r.pc == 0x150E20 || r.pc == 0x150E60 || r.pc == 0x150EE8;
        if (fRoot && r.kind == 0 && depth == 0)
        {
            k.recEntry = r;
            k.ahelper.clear();
            pcRoot = r.pc;
            icallRoot = r.icall;
            depth = 1;
            continue;
        }
        if (!(fRoot && r.kind == 1 && depth > 0 && r.pc == pcRoot && r.icall == icallRoot))
        {
            if (depth > 0) k.ahelper.push_back(r);
            continue;
        }
        depth = 0;
        k.recExit = r;
        k.iCrvPos = k.iEyeMove = k.iEyeCut = k.iCpdefi = 0;
        if (pcRoot != PC_UPDATECM)
        {
            // a push / pop of the stack (outside UpdateCm): replayed natively in the free run
            if (!fFree || !fLoaded) continue;
            const REC& e = k.recEntry;
            CPFREE& q = g_cmpolicies.cpfree;
            if (pcRoot == 0x150EA0) { q.SetTargets(glm::vec3(e.GprVec(5)), glm::vec3(e.GprVec(6)), e.Fpr(12)); continue; }
            if (pcRoot == 0x150E20) { q.SetFocusObject(Tok(e.Gpr(5))); continue; }
            if (pcRoot == 0x150E60) { q.SetTargetObject(Tok(e.Gpr(5))); continue; }
            if (pcRoot == 0x150EE8) { q.SetZoomTarget(e.Fpr(12)); continue; }
            if (pcRoot == PC_SETCMPOLICY)
            {
                CPLCY* p = PcplcyFromCmOffset(e.Gpr(6) - A_CM);
                if (p)
                {
                    SetCmPolicy(&g_cm, static_cast<int32_t>(e.Gpr(5)), p, Tok(e.Gpr(7)), PvFromRam(p, e.Gpr(8), k.rvEntry, true));
                    ++cpush;
                }
            }
            else
            {
                CPLCY* p = PcplcyFromCmOffset(e.Gpr(7) - A_CM);
                RevokeCmPolicy(&g_cm, static_cast<int32_t>(e.Gpr(5)), static_cast<int32_t>(e.Gpr(6)), p, Tok(e.Gpr(8)),
                               PvFromRam(p, e.Gpr(9), k.rvEntry, false));
                ++cpop;
            }
            continue;
        }
        if (!fFree || !fLoaded)
        {
            LoadCmFromRam(k.rvEntry, true);
            fLoaded = true;
        }
        else
        {
            // the reference's inputs: the zones' fields (their path parameter +0x140 is the camera's own)
            for (auto& it : k.mpzone)
            {
                ZONECM* pz = it.second.get();
                const float sPath = pz->sPath;
                PzoneFromRam(it.first, k.rvEntry, true);
                pz->sPath = sPath;
            }
            csync += SyncStack(k.rvEntry);
            // JoltCm (0x14F750), CutCm (0x14D8C0) and SetCmCut (0x14D8D8) are called by other modules too (UpdateCm
            // clears the cut at its end, so a cut at the entry was asked between frames): inputs here
            g_cm.tJolt = k.rvEntry.F32(A_CM + 0x618);
            g_cm.nJolt = k.rvEntry.S32(A_CM + 0x61C);
            const int fCutRef = k.rvEntry.S32(A_CM + 0x300);
            if (fCutRef == 2 && g_cm.fCut < 2) SetCmCut(&g_cm, k.rvEntry.F32(A_CM + 0x304), k.rvEntry.F32(A_CM + 0x308));
            else if (fCutRef == 1) CutCm(&g_cm);
        }
        UpdateCm(&g_cm);
        Compare(st, k.rvExit, fVerbose);
    }
    if (fFree) std::printf("free run: %d pushes and %d pops replayed, %d stack corrections\n", cpush, cpop, csync);
    for (const auto& [ib, e] : st.mpTop) std::printf("  top policy CM +0x%X: %d frames, %d within\n", ib, e.first, e.second);
    std::printf("%s: %d UpdateCm calls, %d within (pos 0.5, matrix 1e-3, fov 1e-4); max |dpos| %.3f (vblank %d), dmat %.2e, "
                "dfov %.2e, clip matrix (relative) %.2e, follow policy angles %.2e; %d helper argument mismatches\n",
                fFree ? "free run" : "per frame", st.c, st.cOk, st.dposMax, st.vbWorst, st.dmatMax, st.dfovMax, st.dclipMax,
                st.dpolMax, k.cmismatchArg);
    return st.c > 0 && st.cOk == st.c ? 0 : 1;
}

// ---- native curves for the checks: the reference's curve (a RAM address) -> the native PATH placed alike
#include "crv.h"
#include "sw.h"
bool FResolveWorld(const std::string& w, FK& fk, int& filter, std::string& err);
namespace
{
SW* s_pswCrv = nullptr;  // (kept to the end: its unload needs the splice runtime the exit has taken down)
const CRV* PcrvFromRam(uint32_t a, const std::vector<uint8_t>& ram)
{
    static std::map<uint32_t, const CRV*> s_mp;
    if (auto it = s_mp.find(a); it != s_mp.end()) return it->second;
    int32_t oid = 0;
    glm::vec3 posRam(0.0f);
    if (a + 0xA0 <= ram.size())
    {
        std::memcpy(&oid, ram.data() + a + 0x18, 4);
        std::memcpy(&posRam, ram.data() + a + 0x90, 12);  // (oids repeat across instances: its local position)
    }
    const CRV* pcrv = nullptr;
    float dBest = 1e30f;
    if (s_pswCrv)
        for (CID cid = S3CID(0x0A); cid >= 0 && cid < g_acls[S3CID(0x0A)].cidLast; ++cid)
        for (LO* plo : s_pswCrv->mpcidaplo[cid])
            if (plo->oid == oid)
                if (const CRV* p = PcrvFromLo(plo))
                {
                    const float d = glm::length(glm::vec3(static_cast<const LOX*>(plo)->mat[3]) - posRam);
                    if (d < dBest) { dBest = d; pcrv = p; }
                }
    // the reference's world matrix (local +0x60 times its record's world, xf +0x40), as instances placed by the
    // streaming proxies are not in the bare native load
    if (pcrv && a + 0xA0 <= ram.size())
    {
        auto rdm = [&](uint32_t b) {
            glm::mat4 m(1.0f);
            for (int r = 0; r < 4; ++r) std::memcpy(&m[r], ram.data() + b + 16 * r, 16);
            return m;
        };
        glm::mat4 mat = rdm(a + 0x60);
        uint32_t xf = 0;
        std::memcpy(&xf, ram.data() + a + 0x50, 4);
        if (xf && xf + 0x80 <= ram.size()) mat = rdm(xf + 0x40) * mat;
        static std::vector<std::unique_ptr<CRV>> s_acrv;
        s_acrv.push_back(std::make_unique<CRV>(*pcrv));
        RecalcCrv(s_acrv.back().get(), mat);
        pcrv = s_acrv.back().get();
    }
    s_mp[a] = pcrv;
    return pcrv;
}
bool FLoadCrvWorld(const std::string& world)
{
    std::string err;
    FK fk;
    int filter = 0;
    if (!FResolveWorld(world, fk, filter, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return false; }
    s_pswCrv = new SW;
    s_pswCrv->filter = filter;
    std::size_t cbRead = 0, cbFile = 0;
    return FLoadWorldFile(s_pswCrv, fk, "", &cbRead, &cbFile);
}
}  // namespace

void BindNativeCurves()
{
    g_cmfollowworld.pfnCrvPos = [](LO* plo, float sv, glm::vec3* ppos, glm::vec3* pdir) {
        if (const CRV* p = PcrvFromRam(AddrOf(plo), s_chk.ramStatic)) EvalCrvS(p, sv, ppos, pdir);
    };
    g_cmfollowworld.pfnCrvNearest = [](LO* plo, const glm::vec3& pos, float sv) {
        float sOut = sv;
        if (const CRV* p = PcrvFromRam(AddrOf(plo), s_chk.ramStatic)) NearestCrvS(p, pos, sv, nullptr, nullptr, nullptr, &sOut);
        return sOut;
    };
    g_cmfollowworld.pfnCrvNearestAll = [](LO* plo, const glm::vec3& pos, float sv) {
        float sOut = sv;
        if (const CRV* p = PcrvFromRam(AddrOf(plo), s_chk.ramStatic)) NearestCrvAll(p, pos, nullptr, nullptr, nullptr, &sOut);
        return sOut;
    };
}
bool FLoadCrvWorldForCheck(const std::string& world) { return FLoadCrvWorld(world); }

// ---- --crv-check TRACE --world W: the native curves (crv.cpp) against the reference's 0x1E6F10 calls in the trace
// (their arguments and outputs: the curve by its oid, the distance, the position and direction written to the stack)
int CrvCheck(const std::string& pathTrace, const std::string& pathRam, const std::string& world)
{
    if (!FLoadCrvWorld(world)) return 2;
    std::vector<uint8_t>& ram = s_chk.ramStatic;
    ram.resize(32u << 20);
    if (std::FILE* pf = std::fopen(pathRam.c_str(), "rb")) { ram.resize(std::fread(ram.data(), 1, ram.size(), pf)); std::fclose(pf); }
    TRACE tr;
    if (!tr.FOpen(pathTrace, "")) return 2;
    std::map<uint32_t, REC> mpEntry;  // by call index
    REC r;
    int c = 0, cOk = 0, cMissing = 0;
    double dposMax = 0, ddirMax = 0;
    while (tr.FRead(r))
    {
        if (r.pc != 0x1E6F10) continue;
        if (r.kind == 0) { mpEntry[r.icall] = r; continue; }
        auto it = mpEntry.find(r.icall);
        if (it == mpEntry.end()) continue;
        const REC& e = it->second;
        const uint32_t a = e.Gpr(4);
        int32_t oid = 0;
        std::memcpy(&oid, ram.data() + a + 0x18, 4);
        const CRV* pcrv = PcrvFromRam(a, ram);
        if (!pcrv) { ++cMissing; mpEntry.erase(it); continue; }
        glm::vec3 pos, dir;
        EvalCrvS(pcrv, e.Fpr(12), &pos, &dir);
        const glm::vec3 posRef = V3Stack(r, e.Gpr(5)), dirRef = V3Stack(r, e.Gpr(6));
        const float dpos = glm::length(pos - posRef), ddir = glm::length(dir - dirRef);
        ++c;
        if (dpos < 0.05f && ddir < 1e-4f) ++cOk;
        else if (c - cOk <= 5)
            std::printf("  curve %d s %.3f: native (%.3f %.3f %.3f) ref (%.3f %.3f %.3f), dir %.2e\n", oid, e.Fpr(12), pos.x, pos.y,
                        pos.z, posRef.x, posRef.y, posRef.z, ddir);
        dposMax = std::max<double>(dposMax, dpos);
        ddirMax = std::max<double>(ddirMax, ddir);
        mpEntry.erase(it);
    }
    std::printf("curves: %d calls of 0x1E6F10, %d within (0.05, 1e-4), max |dpos| %.4f, |ddir| %.2e; %d curves not found\n", c, cOk,
                dposMax, ddirMax, cMissing);
    return c > 0 && c == cOk ? 0 : 1;
}

// ---- --actor-check: Sly's camera definition 0x18A428 (actor.cpp) against the reference's calls
int ActorCheck(const std::string& pathTrace, const std::string& ranges, const std::string& pathRam)
{
    // (state 8's curves: the native world 2's, matched in the static RAM)
    std::vector<uint8_t> ram(32u << 20);
    if (std::FILE* pf = std::fopen(pathRam.c_str(), "rb")) { ram.resize(std::fread(ram.data(), 1, ram.size(), pf)); std::fclose(pf); }
    else ram.clear();
    const bool fCrv = !ram.empty() && FLoadCrvWorld("2");
    TRACE tr;
    if (!tr.FOpen(pathTrace, ranges)) return 2;
    REC r, e;
    bool fIn = false;
    glm::vec4 vPoint(0.0f), w(0.0f);
    int c = 0, cOk = 0;
    double dMax = 0;
    std::map<int, int> mpstate;
    auto readAt = [](const REC& x, const RAMVIEW& rv, uint32_t a, void* pv, uint32_t cb) {
        const uint32_t ib = a - x.Sp();
        if (ib + cb <= sizeof x.abStack) std::memcpy(pv, x.abStack + ib, cb);
        else rv.FRead(a, pv, cb);
    };
    while (tr.FRead(r))
    {
        if (r.pc == 0x18A428 && r.kind == 0) { e = r; fIn = true; vPoint = w = glm::vec4(0.0f); continue; }
        if (!fIn) continue;
        if (r.pc == 0x22D348 && r.kind == 1) { vPoint = r.GprVec(2); continue; }
        if (r.pc == 0x1F4558 && r.kind == 1) { w = r.GprVec(2); continue; }
        if (r.pc != 0x18A428 || r.kind != 1 || r.icall != e.icall) continue;
        fIn = false;
        RAMVIEW rvE, rvX;
        rvE.paranges = rvX.paranges = &tr.aranges;
        rvE.prec = &e;
        rvX.prec = &r;
        const uint32_t a = e.Gpr(4), pf = e.Gpr(5);
        if (a != 0xFB1580) continue;  // (Paris's Sly: the ranges' object)
        SLY act;
        static XP s_xpFloor;
        act.jump.f28 = rvE.S32(a + 0x2E8);
        if (fCrv && rvE.U32(a + 0xF5C)) act.pcrvF50 = PcrvFromRam(rvE.U32(a + 0xF5C), ram);
        act.sCrvF50 = rvE.F32(a + 0xF60);
        act.gCrvF50 = rvE.F32(a + 0xF64);
        act.pxpFloor = rvE.U32(a + 0x208) ? &s_xpFloor : nullptr;
        act.posFloor.z = rvE.F32(a + 0x238);
        act.vecBase = rvE.V4(a + 0x240);
        act.jump.state = rvE.S32(a + 0x2DC);
        act.f434 = rvE.S32(a + 0x434);
        act.nSub4CC = rvE.S32(a + 0x4CC);
        act.nSubDF4 = rvE.S32(a + 0xDF4);
        act.nSubF54 = rvE.S32(a + 0xF54);
        act.f1044 = rvE.S32(a + 0x1044);
        act.sHeight = rvE.F32(a + 0x1048);
        act.zFocus = rvE.F32(a + 0x104C);
        act.zFocusSmooth = rvE.F32(a + 0x1050);
        act.posCmTarget = rvE.V4(a + 0x1060);
        act.vecOffset = rvE.V3(a + 0x1070);
        act.state = rvE.S32(a + 0x18C8);
        act.tActive = rvE.F32(a + 0x1AA4);
        ACTORPOSE pose;
        const uint32_t xf = rvE.U32(a + 0x60);
        pose.mat = glm::mat3(rvE.V3(xf + 0x40), rvE.V3(xf + 0x50), rvE.V3(xf + 0x60));
        pose.pos = rvE.V3(xf + 0x70);
        pose.vPoint = glm::vec3(vPoint);
        pose.w = glm::vec3(w);
        ACTORCMCTX ctx;
        ctx.fCut = rvE.S32(A_CM + 0x300) != 0;
        ctx.fFocus = rvE.U32(A_CM + 0x260) == a;
        ctx.t = rvE.F32(A_CLOCK);
        ctx.dt = rvE.F32(A_CLOCK + 4);
        const uint32_t aOther = rvE.U32(a + 0x18C4);
        if (aOther) ctx.zOther = rvE.F32(rvE.U32(aOther + 0x60) + 0x78);
        CPDEFI cp;
        GetSlyCpdefi(act, pose, ctx, &cp);
        uint8_t ab[0x40];
        readAt(r, rvX, pf, ab, sizeof ab);
        std::vector<uint8_t> v(ab, ab + sizeof ab);
        RAMVIEW rvp;
        rvp.pram = &v;
        const CPDEFI ref = CpdefiFromRam(rvp, 0);
        const float d = std::max({glm::length(glm::vec3(cp.posBase - ref.posBase)), glm::length(glm::vec3(cp.vBase - ref.vBase)),
                                  std::fabs(RadNormalize(cp.radHome - ref.radHome)), std::fabs(cp.sAdjust - ref.sAdjust),
                                  std::fabs(cp.tMoveLast - ref.tMoveLast), std::fabs(act.zFocus - rvX.F32(a + 0x104C)),
                                  std::fabs(act.zFocusSmooth - rvX.F32(a + 0x1050)),
                                  glm::length(act.vecOffset - rvX.V3(a + 0x1070)), std::fabs(act.tActive - rvX.F32(a + 0x1AA4))});
        const bool fOk = d < 1e-3f && cp.grfcpd == ref.grfcpd && cp.cfk == ref.cfk;
        ++c;
        ++mpstate[act.state];
        if (fOk) ++cOk;
        else if (c - cOk <= 8)
            std::printf("  call %u (vb %u) state %d sub %d/%d/%d/%d ground %d cut %d focus %d: native pos (%.3f %.3f %.3f) v (%.2f %.2f %.2f) "
                        "rad %.4f s %.3f grf %X; ref pos (%.3f %.3f %.3f) v (%.2f %.2f %.2f) rad %.4f s %.3f grf %X; d %.4f\n",
                        e.icall, e.vb, act.state, act.jump.state, act.nSub4CC, act.nSubDF4, act.nSubF54, act.pxpFloor ? 1 : 0, ctx.fCut, ctx.fFocus,
                        cp.posBase.x, cp.posBase.y, cp.posBase.z, cp.vBase.x, cp.vBase.y, cp.vBase.z, cp.radHome, cp.sAdjust, cp.grfcpd,
                        ref.posBase.x, ref.posBase.y, ref.posBase.z, ref.vBase.x, ref.vBase.y, ref.vBase.z, ref.radHome, ref.sAdjust,
                        ref.grfcpd, d);
        dMax = std::max<double>(dMax, d);
    }
    std::printf("actor: %d calls of 0x18A428, %d within 1e-3 (max %.5f); states:", c, cOk, dMax);
    for (auto [s, n] : mpstate) std::printf(" %d:%d", s, n);
    std::printf("\n");
    return c > 0 && c == cOk ? 0 : 1;
}

// The Sly harness's take-over (slycheck.cpp): the camera's state and the follow policy's from the reference's RAM,
// the native objects, zones and policy stack kept
void ImportCmStateFromRam(const std::vector<uint8_t>& ram)
{
    RAMVIEW rv;
    rv.pram = &ram;
    LO* ploCur = g_cm.ploCur;
    LO* psoPrev = g_cm.psoFocusPrev;
    LoadCmFromRam(rv, false);
    g_cm.ploCur = ploCur;
    g_cm.psoFocusPrev = psoPrev;
    LoadFollowFromRam(rv);
}
