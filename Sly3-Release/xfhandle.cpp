// xfhandle.cpp: a script's handle to one transform record of an ALO (the original's class 0x95, 0x30 bytes from the
// heap 0x3E15CC, vtable 0x2CA2C0: +0x20 the object, +0x24 the record; module 0x1F8510..0x1F8BF0). An ALO's option 194
// (thunk 0x1014E8) makes one; its properties: 1059 the record's float +0xA8 (the arrival time of the modes 9 / 17,
// 0x1F88D8 resolves it and re-arms the record on a write), the methods 1060 (a position controller toward a world
// point, 0x1F8A78), 1061 (toward a world rotation, 0x1F8AD8) and 1062 (a look-along-the-motion controller, 0x1F8B58).
// E.g. the Ep1 heist harpoon (world 2, block 123952): its flight looks along its motion (1062 on its rotation record),
// and the hit moves it onto the sign's point 142374 (1060 mode 17 by the time 1059, 1061 mode 7).
#include "xfhandle.h"

#include "alo.h"
#include "clock.h"
#include "lo.h"
#include "objmethod.h"
#include "sw.h"
#include "xf.h"

#include <cstdio>
#include <cstdlib>

extern SW* g_psw;
glm::vec3 EulerFromMat(const glm::mat3& m);  // xfmath.h (0x11BFF0)

namespace
{
bool FPrint()
{
    static const bool s_f = std::getenv("SLY2N_XFHPRINT") != nullptr;
    return s_f;
}

bool FLive(const XFBASIC* p)
{
    if (!p->palo || !p->pxf || !g_psw || p->worldSerial != g_psw->serial) return false;
    for (const auto& up : p->palo->axf)
        if (up.get() == p->pxf) return true;
    return false;
}

// orig: 0x0022FC48 (0, the record's parent record, v): a world point into the parent record's frame
glm::vec4 PosToParent(const XF* pxf, const glm::vec3& pos)
{
    const XF* ppar = pxf->pxfRef;
    if (!ppar) return glm::vec4(pos, 0.0f);
    const glm::vec4 v = glm::inverse(ppar->matWorld) * glm::vec4(pos, 1.0f);
    return glm::vec4(glm::vec3(v), 0.0f);
}

// orig: 0x001F8910: the record's first controller of the kind k: mode -1 removes (and frees) it; else its priority
// (re-sorted), and when it drives the record, the record's mode n (0x2303A0) re-armed (0x230598)
CTRL* PctrlFindXfh(XFBASIC* p, int k, int n, int m)
{
    for (CTRL* pe = p->pxf->pctrlFirst; pe; pe = pe->pnext)
    {
        if (pe->kind != k) continue;
        if (n == -1)
        {
            RemoveXfCtrl(p->palo, p->pxf, pe);  // 0x230100
            delete pe;                          // 0x12E210
            return nullptr;
        }
        if (m != pe->prio)
        {
            pe->prio = m;
            SortXfCtrls(p->palo, p->pxf);  // 0x2301D0
        }
        if (pe == p->pxf->pctrlFirst)
        {
            SetXfMode(p->palo, p->pxf, n);   // 0x2303A0
            HoldXfMode(p->palo, p->pxf);     // 0x230598
        }
        return pe;
    }
    return nullptr;
}

// orig: 0x001F89E0: the position controller (kind 1) of mode n, priority m at the value q (in the record's frame)
void SetXfhPos(XFBASIC* p, int n, int m, const glm::vec4& q)
{
    if (CTRL* r = PctrlFindXfh(p, 1, n, m))
    {
        r->mode = n;                                    // +0x14
        if (auto* pp = dynamic_cast<CTRLPOS*>(r)) SetCtrlPos(pp, q);  // 0x12D108
    }
    else if (n != -1)
        PctrlNewPos(p->palo, p->pxf, m, n, 2, q);  // 0x12DC18
}

CRef RefVoid() { return CRef(TAGK_Void); }

// orig: 0x0010E5D8 -> 0x001F8A78 (mode, priority, a world point)
CRef RefXfhMovePos(BASIC* pb, int c, CRef* a)
{
    auto* p = dynamic_cast<XFBASIC*>(pb);
    if (!p || !FLive(p) || c < 3) return RefVoid();
    const int n = a[0].RefCoerceS32().m_n, m = a[1].RefCoerceS32().m_n;
    const glm::vec4 q = PosToParent(p->pxf, a[2].GetVector());
    if (FPrint())
        std::printf("xfh t %.3f: %d record kind %d pos mode %d prio %d -> %.1f %.1f %.1f\n", g_clock.t, p->palo->oid, p->pxf->kind, n, m,
                    q.x, q.y, q.z);
    SetXfhPos(p, n, m, q);
    if (FPrint())
        std::printf("  -> record mode %d state %d vel %.1f %.1f %.1f head %d\n", p->pxf->mode, p->pxf->state, p->pxf->vel.x, p->pxf->vel.y,
                    p->pxf->vel.z, p->pxf->pctrlFirst ? p->pxf->pctrlFirst->kind : -1);
    return RefVoid();
}

// orig: 0x0010E690 -> 0x001F8AD8 (mode, priority, a world rotation: into the parent record's frame (0x22FD38),
// as Euler angles (0x11BFF0))
CRef RefXfhMoveRot(BASIC* pb, int c, CRef* a)
{
    auto* p = dynamic_cast<XFBASIC*>(pb);
    if (!p || !FLive(p) || c < 3) return RefVoid();
    const int n = a[0].RefCoerceS32().m_n, m = a[1].RefCoerceS32().m_n;
    glm::mat3 mat = a[2].m_tagk == TAGK_Matrix ? glm::mat3(a[2].GetMatrix()) : glm::mat3(1.0f);
    if (const XF* ppar = p->pxf->pxfRef) mat = glm::transpose(glm::mat3(ppar->matWorld)) * mat;
    const glm::vec3 e = EulerFromMat(mat);
    if (FPrint())
        std::printf("xfh t %.3f: %d record kind %d rot mode %d prio %d -> %.3f %.3f %.3f\n", g_clock.t, p->palo->oid, p->pxf->kind, n, m, e.x,
                    e.y, e.z);
    SetXfhPos(p, n, m, glm::vec4(e, 0.0f));
    return RefVoid();
}

// orig: 0x0010E758 -> 0x001F8B58 (mode, priority, the lead's two floats): the look-along-the-motion controller
// (kind 4, 0x12DCA0: +0x1C / +0x20)
CRef RefXfhLook(BASIC* pb, int c, CRef* a)
{
    auto* p = dynamic_cast<XFBASIC*>(pb);
    if (!p || !FLive(p) || c < 4) return RefVoid();
    const int n = a[0].RefCoerceS32().m_n, m = a[1].RefCoerceS32().m_n;
    const float f = a[2].GAsFloat(), g = a[3].GAsFloat();
    if (FPrint()) std::printf("xfh t %.3f: %d record kind %d look mode %d prio %d (%g %g)\n", g_clock.t, p->palo->oid, p->pxf->kind, n, m, f, g);
    if (CTRL* r = PctrlFindXfh(p, 4, n, m))
    {
        r->mode = n;
        if (auto* pl = dynamic_cast<CTRLLOOKMOTION*>(r))
        {
            pl->job.gLo = f;  // +0x1C
            pl->job.gHi = g;  // +0x20
        }
    }
    else if (n != -1)
    {
        // orig: 0x0012DCA0 (0x12DB50(4, ..): kind 4, prio m, mode n; then added, 0x230018)
        auto* pl = new CTRLLOOKMOTION();
        pl->kind = 4;
        pl->palo = p->palo;
        pl->pxf = p->pxf;
        pl->prio = m;
        pl->mode = n;
        pl->job.kind = 4;
        pl->job.gLo = f;
        pl->job.gHi = g;
        AddXfCtrl(p->palo, p->pxf, pl);
    }
    return RefVoid();
}

CRef RefMethodOf(XFBASIC* p, PFNTHUNK pfn, int creq)
{
    auto pm = std::make_shared<CMethod>();
    pm->m_pbasic = p;
    pm->m_pfnthunk = pfn;
    pm->m_crefReq = creq;
    CRef r;
    r.SetMethod(pm);
    return r;
}
}  // namespace

// orig: 0x001014E8 -> 0x001F8510 (an ALO's option 194 (k, kind): its first record of that kind (+0x94) and, unless k is
// -1, that tag (+0x9C); nil when none)
CRef RefXfHandle(LO* plo, const std::vector<CRef>& a)
{
    CRef r;
    r.SetBasic(nullptr);
    ALO* palo = dynamic_cast<ALO*>(plo);
    if (!palo) return r;
    const int k = NArg(a, 0, -1), m = NArg(a, 1, 0);
    for (const auto& up : palo->axf)
        if (up && up->kind == m && (k == -1 || up->tag == k))
        {
            auto p = std::make_shared<XFBASIC>();
            p->palo = palo;
            p->pxf = up.get();
            p->worldSerial = g_psw ? g_psw->serial : 0;
            r.SetOwnedBasic(std::move(p));
            break;
        }
    if (FPrint()) std::printf("xfh: %d option 194 (%d %d): %s\n", plo->oid, k, m, r.m_pbasic ? "found" : "none");
    return r;
}

// The handle's properties (class 0x95's table: 1059 = EOPID 1227, a float field at the record's +0xA8 through the
// resolver 0x1F88D8; 1060 / 1061 / 1062 = EOPIDs 1228 / 1229 / 1230, methods)
CRef RefXfHandleOption(int bifk, XFBASIC* p, int optid, const CRef* pref)
{
    if (!FLive(p)) return RefVoid();
    if (bifk == 111)
    {
        switch (optid)
        {
        case 1059: { CRef r; r.SetF32(p->pxf->tTarget); return r; }
        case 1060: return RefMethodOf(p, RefXfhMovePos, 3);
        case 1061: return RefMethodOf(p, RefXfhMoveRot, 3);
        case 1062: return RefMethodOf(p, RefXfhLook, 4);
        default: return RefVoid();
        }
    }
    if (bifk == 110 && pref && optid == 1059)
    {
        p->pxf->tTarget = pref->GAsFloat();
        HoldXfMode(p->palo, p->pxf);  // (0x1F88D8 with 1: the write re-arms the record, 0x230598)
        if (FPrint()) std::printf("xfh t %.3f: %d record kind %d time %.3f\n", g_clock.t, p->palo->oid, p->pxf->kind, p->pxf->tTarget);
    }
    return RefVoid();
}

namespace
{
struct INITXFHANDLE
{
    INITXFHANDLE() { RegisterObjectMethod(0x1014E8, RefXfHandle); }
} s_initxfhandle;
}  // namespace
