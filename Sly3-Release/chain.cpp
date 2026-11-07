// Chains (ropes) of transform records (orig module 0x21C6E8-0x21D120): the segments' rest state at the load (the
// part of 0x21C888 after the read, the kind-3 controller 0x21C858 putting its record in mode 16) and the simulation
// of mode 16 (0x21CB18: the segments keep their lengths, bend at most the limit, are pulled straight, damped and
// pulled by gravity; the end records turn toward their segment). Ported from the reference's m_21C6E8_p3.cpp.

#include "mat.h"
#include "so.h"
#include "anim.h"
#include "clock.h"
#include "xfphys.h"
#include "xf.h"

#include <cmath>


namespace
{
constexpr float EPS = 1e-4f;
inline float Dot3(const glm::vec3& a, const glm::vec3& b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }
inline float Len3(const glm::vec3& a) { return std::sqrt(std::fabs(Dot3(a, a))); }
}

namespace
{
// The chain's controller of each segment's record (orig 0x21C888: 0x12E1F0(0x20) with the vtable D_002DB338, kind 3,
// priority -1, mode 16, +0x1C the segment; added by 0x230018). Its start 0x21C858 puts the record in mode 16 (0x12CE98)
// and the segment back in the record's +0xA8 (the other modes use +0xA8 for themselves). It stays under the record's
// other controllers: when an animation's channel (kind 0, priority 0) lets go of the record, the chain takes it again.
// (Natively the mode was set once at the load; a record an animation had driven stayed in its seek / snap mode, so
// Sly's tail and ears stopped following him (R10).)
struct CTRLCHAIN : CTRL
{
    ALOCHAINE* pe = nullptr;  // +0x1C
    void Start() override     // orig 0x21C858
    {
        CTRL::Start();
        pxf->pvA8 = pe;
        // (natively the parameter can still point at the time of an animation that is gone; mode 16 reads none of
        // them (0x22DD00 only fetches from this controller, which does nothing): dropped)
        ResetXfParams(pxf);
    }
};
}  // namespace

// The part of 0x21C888 after each chain is read: each segment's rest rows, the controller that holds its record in
// mode 16 (CTRLCHAIN above), then the offsets to the next segment (in the record's frame; the last one: the previous
// offset, or the chain's rest offset) and the world positions.
void InitAloChains(ALO* palo)
{
    for (ALOCHAIN& c : palo->achain)
    {
        const int n = int(c.ae.size());
        for (ALOCHAINE& e : c.ae)
        {
            e.pchain = &c;
            if (!e.pxf) continue;
            e.matRest = Mat3Of(e.pxf->mat);
            bool fHas = false;
            for (CTRL* q = e.pxf->pctrlFirst; q; q = q->pnext)
                if (auto* pc = dynamic_cast<CTRLCHAIN*>(q); pc && pc->pe == &e) fHas = true;
            if (!fHas)
            {
                auto* pctrl = new CTRLCHAIN();  // (the controllers live as long as the world, as xf.cpp's)
                pctrl->kind = 3;
                pctrl->palo = palo;
                pctrl->pxf = e.pxf;
                pctrl->prio = -1;
                pctrl->mode = 16;
                pctrl->pe = &e;
                AddXfCtrl(palo, e.pxf, pctrl);  // 0x230018 (the head: started, mode 16)
            }
        }
        for (int j = 0; j < n; j++)
        {
            ALOCHAINE& e = c.ae[j];
            if (j < n - 1)
                e.vecOffset = c.ae[j + 1].pxf ? PosConvertXfs(nullptr, e.pxf, PosOf(c.ae[j + 1].pxf->matWorld)) : g_vecZero;
            else if (j > 0)
                e.vecOffset = VecConvertXfs(c.ae[j - 1].pxf, e.pxf, c.ae[j - 1].vecOffset);
            else
                e.vecOffset = c.vec30;
            e.pos = PosConvertXfs(e.pxf, nullptr, e.vecOffset);
        }
    }
}

// orig: 0x0021CB18 func_0021CB18 (mode 16 of a chain segment's record: once per frame the chain is simulated from this
// segment on (n substeps); then the end record's rotation turns its rest direction toward the segment)
void UpdateXfChain(ALO* pa, void* pv)
{
    ALOCHAINE* rec = static_cast<ALOCHAINE*>(pv);
    if (!rec || !rec->pchain || !rec->pxf) return;
    ALOCHAIN* rope = rec->pchain;
    XF* end = rec->pxf;
    XF* base = end->pxfRef;
    const float t = g_clock.t;
    const glm::mat4 matBase = base ? base->matWorld : g_matIdentity4;
    if (!(t == rope->tLast))
    {
        const int idx = int(rec - rope->ae.data());
        glm::vec3 V0, P0, C0;
        if (idx == 0)
        {
            const glm::vec3 anchor = PosOf(end->matWorld);
            if (GrfUpdOfLo(pa) & 0x10u)
                V0 = base ? VelXfPoint(base, nullptr, anchor) : g_vecZero;
            else
                V0 = (anchor - rope->posAnchor) * (1.0f / g_clock.dt);
            P0 = anchor;
            rope->posAnchor = anchor;
            C0 = (Mat3Of(matBase) * rec->matRest) * rec->vecOffset;
        }
        else
        {
            int j = 0;
            for (; j < idx; j++)
            {
                ALOCHAINE& s = rope->ae[j];
                s.pos = PosConvertXfs(s.pxf, nullptr, s.vecOffset);
                s.v = s.pxf ? VelXfPoint(s.pxf, nullptr, s.pos) : g_vecZero;
            }
            V0 = rope->ae[idx - 1].v;
            P0 = rope->ae[idx - 1].pos;
            if (j < 2) C0 = P0 - PosOf(matBase);
            else C0 = P0 - rope->ae[idx - 2].pos;
        }
        const int n0 = rope->n8;
        const float h = g_clock.dt / static_cast<float>(n0);
        for (int k = 0; k < n0; k++)
        {
            const float c = static_cast<float>(k - n0 + 1) * h;
            glm::vec3 a1 = C0;
            glm::vec3 vf11 = V0;
            glm::vec3 vf10 = P0 + V0 * c;
            for (int i = idx; i < int(rope->ae.size()); i++)
            {
                ALOCHAINE& seg = rope->ae[i];
                glm::vec3 vf8 = seg.pos;
                glm::vec3 vf7 = seg.v;
                glm::vec3 vf9 = vf8 - vf10;
                const glm::vec3 vf12 = vf7 - vf11;
                glm::vec3 axis;
                const float ang = RadBetweenVectors(vf9, a1, &axis);
                const float lim = rope->ag[3];
                if (lim < ang)
                {
                    const glm::mat3 R = BuildRotateMatrixAxis(ang - lim, axis);
                    vf9 = R * vf9;
                    vf8 = vf10 + vf9;
                }
                float fd = -rope->ag[1];
                float fk = rope->ag[0] * ang;
                float fe = -rope->ag[2];
                fd = fd * h;
                const glm::vec3 bend = glm::cross(axis, vf9);
                fk = fk * h;
                fe = fe * h;
                fd = std::max(-1.0f, fd);
                fd = fd + 1.0f;
                fe = std::max(-1.0f, fe);
                vf7 = ((vf7 * fd + vf12 * fe) + bend * fk) + rope->vec20 * h;
                vf8 = vf8 + vf7 * h;
                const glm::vec3 vf3 = vf8 - vf10;
                const float L = Len3(vf3);
                const glm::vec3 dir = (L < EPS) ? g_normalX : vf3 * (1.0f / L);
                const float d1 = Dot3(vf7, dir);
                const float d2 = Dot3(vf11, dir);
                const glm::vec3 tang = vf7 - dir * d1;
                const glm::vec3 rad = dir * d2;
                vf7 = rad + tang;
                const float Ls = Len3(seg.vecOffset);
                seg.v = vf7;
                vf8 = vf10 + dir * Ls;
                vf11 = vf7;
                seg.pos = vf8;
                vf10 = vf8;
                a1 = vf9;
            }
        }
        rope->tLast = g_clock.t;
    }
    const glm::vec3 p = PosConvertXfs(nullptr, base, rec->pos);
    const glm::vec3 to = p - PosOf(end->mat);
    const glm::vec3 from = rec->matRest * rec->vecOffset;
    const glm::mat3 R = BuildRotateVectorsMatrix(from, to);
    SetMat3(&end->mat, R * rec->matRest);
    end->nStamp = ++g_cXfStamp;
}
