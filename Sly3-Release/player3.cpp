// player3.cpp: Sly 3's player in the native game (player3.h): the development start in gameplay and the per-frame
// trace. The player's own code is the copied Sly 2 player (sly.cpp, actor.cpp, step*.cpp, jump.cpp ...) dispatched on
// Sly 3's classes through vtmap3.cpp, with the Sly 3 differences ported from the Sly 3 asm (orig3 comments).
#include "player3.h"
#include "combo3.h"
#include "binoc3.h"

#include "game/sly3.h"
#include "game/scptgame3.h"

#include "actor.h"
#include "activate.h"
#include "alo.h"
#include "clock.h"
#include "cm.h"
#include "crv.h"
#include "jt.h"
#include "lo.h"
#include "po.h"
#include "sly.h"
#include "sm.h"
#include "asega.h"
#include "aseg.h"
#include "cls.h"
#include "so.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "worldaccess.h"
#include "xf.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sly3
{
namespace
{
FILE* s_ptrace = nullptr;
}

LO* PloSly3(SW* psw)
{
    if (!psw) return nullptr;
    const auto& a = psw->mpcidaplo[S3CID(0x35)];
    return a.empty() ? nullptr : a[0];
}

// The reference at "Sly in control" (Sly3-research/ref3/boot v.5600.ram; PCSX2 st4 to 0.01): Sly's root record
// (Sly +0x48 -> +0x70) and his heading (STEP +0x1BC).
const glm::vec3 c_posSpawn(-6713.0708f, -14376.3467f, -2913.2537f);
constexpr float c_radSpawn = 1.20430f;

// SLY3_JTLIST=1: the world's player targets (ledges, pipes, spires, ... : the Sly 3 classes of the JT kinds) and what
// the copied Sly 2 lists (jt.cpp AploJtList) hold
void ListJt3(SW* psw)
{
    if (std::getenv("SLY3_JTLIST")[0] == '2')
        for (int cid : {0x0A, 0x0B, 0x0C, 0x19, 0x20, 0x21, 0x24, 0x25, 0x26})
            for (LO* p : psw->mpcidaplo[cid])
            {
                std::printf("jt3: all cid 0x%02X oid %6d inworld %d removed %d st %d:", cid, p->oid, p->fInWorld, p->fRemoved, p->st);
                for (LO* q = p->paloParent; q; q = q->paloParent)
                    std::printf(" <- %d (cid 0x%X inworld %d removed %d)", q->oid, q->Cid(), q->fInWorld, q->fRemoved);
                std::printf("\n");
            }
    static const char* c_aszJtl[JTL_Max] = {"point", "pipe", "rail", "spire", "hide", "ledge", "wall", "swing", "use", "response", "climbsurf", "crawlhook"};
    for (int j = 0; j < JTL_Max; ++j)
        for (LO* p : AploJtList(psw, static_cast<JTL>(j)))
        {
            const glm::vec3 pos(MatJtLo(p)[3]);
            const JTDESC d = JtDescOf(p, static_cast<JTL>(j));
            std::printf("jt3: %-9s cid 0x%02X oid %6d at %9.1f %9.1f %9.1f mask 0x%X id %d prio %d\n", c_aszJtl[j], p->Cid(), p->oid, pos.x, pos.y, pos.z,
                        d.mask, d.id, d.prio);
            if (std::getenv("SLY3_JTLIST")[0] == '3')  // (3: the object's stored option fields)
                for (const auto& [key, u] : p->afield)
                {
                    float g;
                    std::memcpy(&g, &u, 4);
                    std::printf("jt3:     field getter 0x%X +0x%X = 0x%X (%g)\n", unsigned(key >> 32), unsigned(key), u, g);
                }
            if (const CRV* pcrv = PcrvFromLo(p); pcrv && !pcrv->apos.empty())
                for (const glm::vec3& v : pcrv->apos)
                {
                    const glm::vec3 w(pcrv->mat * glm::vec4(v, 1.0f));
                    std::printf("jt3:     point %9.1f %9.1f %9.1f\n", w.x, w.y, w.z);
                }
        }
}

bool FStartPlayer3(SW* psw)
{
    const char* sz = std::getenv("SLY3_START");
    if (!sz || !*sz || *sz == '0') return false;
    LO* plo = PloSly3(psw);
    if (!plo)
    {
        std::fprintf(stderr, "player3: no Sly (cid 0x%X) in this world\n", S3CID(0x35));
        return false;
    }
    ALO* palo = static_cast<ALO*>(plo);
    glm::vec3 pos = c_posSpawn;
    float rad = c_radSpawn;
    // SLY3_START=x,y,z[,rad]: another place
    if (std::strchr(sz, ','))
    {
        float x, y, z, r = rad;
        if (std::sscanf(sz, "%f,%f,%f,%f", &x, &y, &z, &r) >= 3) pos = glm::vec3(x, y, z), rad = r;
    }
    // (the development start skips the prologue's scripts (the script code: the attract, the title, the binocucom), so
    // their cameras do not hold the view; SLY3_START_SCRIPTS=1 keeps them)
    if (!std::getenv("SLY3_START_SCRIPTS")) ResetScripts3();
    // SLY3_ADDLO=oid,..: objects the development start adds (the ones the skipped scripts / tasks add: e.g. i_trainer's
    // training stations 163607 163795 129582 83655 164129, in the world in the reference from vblank 2400)
    if (const char* szAdd = std::getenv("SLY3_ADDLO"))
        for (const char* q = szAdd; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
        {
            const int oid = std::atoi(q);
            for (const auto& up : psw->aploAll)
                if (up->oid == oid && up->fRemoved) AddLo(psw, up.get());
        }
    // (the prologue removes him at the start; its binocucom task (the script code) adds him back: orig 0x1C7530 AddLo)
    if (plo->fRemoved) AddLo(psw, plo);
    if (std::getenv("SLY3_STARTDBG"))
        if (XF* pxf = PxfPos(palo))
            std::printf("player3: before: Sly's record local %.1f %.1f %.1f world %.1f %.1f %.1f; ref world %.1f %.1f %.1f local %.1f %.1f %.1f\n",
                        pxf->mat[3].x, pxf->mat[3].y, pxf->mat[3].z, pxf->matWorld[3].x, pxf->matWorld[3].y, pxf->matWorld[3].z,
                        pxf->pxfRef ? pxf->pxfRef->matWorld[3].x : 0.f, pxf->pxfRef ? pxf->pxfRef->matWorld[3].y : 0.f,
                        pxf->pxfRef ? pxf->pxfRef->matWorld[3].z : 0.f, pxf->pxfRef ? pxf->pxfRef->mat[3].x : 0.f,
                        pxf->pxfRef ? pxf->pxfRef->mat[3].y : 0.f, pxf->pxfRef ? pxf->pxfRef->mat[3].z : 0.f);
    STEP* p = PstepFromLo(plo);
    const float c = std::cos(rad), s = std::sin(rad);
    const glm::mat3 mat(glm::vec3(c, s, 0.0f), glm::vec3(-s, c, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    if (p)
    {
        SetStepRotation(p, mat);
        SetStepPos(p, pos);
    }
    else
    {
        SetAloRotation(palo, mat);
        SetAloPos(palo, pos);
    }
    EnablePo(psw, palo, 1);
    // the camera as the reference's at that moment (its follow policy +0xA0 pan -2.1146, tilt 0.1974 over the beach
    // zone's entry): a cut to that pan (the follow's g_cm.fCut 2 path)
    {
        float radPan = -2.1146f;
        if (const char* szPan = std::getenv("SLY3_START_PAN")) radPan = float(std::atof(szPan));
        g_cm.fCut = 2;
        g_cm.radCutPan = radPan;
        g_cm.radCutTilt = 0.1974f + std::atan(-0.25f * g_cm.yScreenRange);
    }
    if (std::getenv("SLY3_STARTDBG"))
        std::printf("player3: Sly iupd %d grfUpd 0x%x removed %d inworld %d st %d nodeActive %d parent %p step %p sly state %d\n", plo->iupd,
                    plo->grfUpd, plo->fRemoved, plo->fInWorld, plo->st, PloRoot(plo)->fNodeActive, (void*)plo->paloParent, (void*)p,
                    PslyFromLo(plo) ? PslyFromLo(plo)->state : -9);
    if (std::getenv("SLY3_STARTDBG"))
        if (SLY* psly = PslyFromLo(plo); psly && psly->psmIdle)
        {
            std::printf("player3: idle SM %d states:", psly->psmIdle->oid);
            for (const auto& [oid, next] : psly->psmIdle->anode) std::printf(" 0x%X", oid);
            std::printf("\n");
        }
    DumpCombos3(psw);
    if (std::getenv("SLY3_PARTDBG"))
        for (const auto& [ib, plop] : palo->apart)
        {
            const ASEG* pa = dynamic_cast<const ASEG*>(plop);
            std::printf("player3: part +0x%X%s -> %d cid 0x%X%s", ib & ~FN3_TAG, (ib & FN3_TAG) ? " (Sly 3)" : "", plop ? plop->oid : -1,
                        plop ? plop->Cid() : -1, pa ? "" : "\n");
            if (pa) std::printf(" ASEG %.3f s, %zu channels\n", PasegdOf(const_cast<ASEG*>(pa)) ? PasegdOf(const_cast<ASEG*>(pa))->tMax : -1.0f,
                                PasegdOf(const_cast<ASEG*>(pa)) ? PasegdOf(const_cast<ASEG*>(pa))->achn.size() : 0);
        }
    if (std::getenv("SLY3_STARTDBG"))
        if (XF* pxf = PxfPos(palo))
            std::printf("player3: Sly's record: local %.1f %.1f %.1f world %.1f %.1f %.1f ref %p (owner %d cid 0x%X) kind %d\n", pxf->mat[3].x,
                        pxf->mat[3].y, pxf->mat[3].z, pxf->matWorld[3].x, pxf->matWorld[3].y, pxf->matWorld[3].z, (void*)pxf->pxfRef,
                        pxf->pxfRef && pxf->pxfRef->palo ? pxf->pxfRef->palo->oid : -1, pxf->pxfRef && pxf->pxfRef->palo ? pxf->pxfRef->palo->Cid() : -1,
                        pxf->kind);
    std::printf("player3: Sly (oid %d) placed at %.2f %.2f %.2f heading %.4f, in control\n", plo->oid, pos.x, pos.y, pos.z, rad);
    if (std::getenv("SLY3_JTLIST")) ListJt3(psw);
    return true;
}

// orig3 0x1A9260 (the player's camera kind, for Camera Options...): 0 the camera, 1 the binocucom (its window open on
// the current player), 2 the grapple-cam, 3 a turret, 4 strafing, 5 swimming, 6 the harpoon. TODO(player): 2..6 with
// their moves (none on the goal path so far)
int NPlayerCamKind3(SW* psw)
{
    LO* plo = psw ? PloPoCurrent(psw) : nullptr;
    if (plo && NBinocPanelState3(plo) >= 0) return 1;
    return 0;
}

void SetPosPlayer3(SW* psw, int frame)
{
    static const char* s_sz = std::getenv("SLY3_SETPOS");
    if (!s_sz) return;
    for (const char* p = s_sz; p && *p; p = std::strchr(p, ';') ? std::strchr(p, ';') + 1 : nullptr)
    {
        int f;
        float x, y, z;
        if (std::sscanf(p, "%d:%f %f %f", &f, &x, &y, &z) != 4 || f != frame) continue;
        LO* plo = PloSly3(psw);
        if (!plo) return;
        ALO* palo = static_cast<ALO*>(plo);
        if (STEP* pstep = PstepFromLo(plo)) SetStepPos(pstep, glm::vec3(x, y, z));
        else SetAloPos(palo, glm::vec3(x, y, z));
        SetAloVelocity(palo, glm::vec3(0.0f));
        SetAloAngVelocity(palo, glm::vec3(0.0f));
        std::printf("player3: frame %d: Sly moved to %.1f %.1f %.1f\n", frame, x, y, z);
    }
}

void TracePlayer3(SW* psw, int frame)
{
    static const char* s_szTrace = std::getenv("SLY3_PLAYERTRACE");
    if (!s_szTrace) return;
    if (!s_ptrace)
    {
        s_ptrace = std::fopen(s_szTrace, "w");
        if (!s_ptrace) return;
        std::fprintf(s_ptrace, "f,t,state,substate,jstate,x,y,z,vx,vy,vz,heading,mode,cx,cy,cz,cmpol,u\n");
    }
    LO* plo = PloSly3(psw);
    SLY* p = PslyFromLo(plo);
    if (!plo || !p) return;
    const glm::vec3 pos = PosLoWorld(plo);
    const glm::vec3 v = VelAloPos(static_cast<ALO*>(plo));
    std::fprintf(s_ptrace, "%d,%.4f,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.5f,%d,%.2f,%.2f,%.2f,%d,%.3f\n", frame, g_clock.t,
                 FCombo3Attacking(p) ? 99 : p->state, p->substate, p->jump.state, pos.x, pos.y, pos.z, v.x, v.y, v.z, p->radHeading, p->mode, g_cm.pos.x, g_cm.pos.y,
                 g_cm.pos.z, g_cm.nCur, PpadctlOfSly3(p) ? PpadctlOfSly3(p)->uJoy : -1.0f);
    std::fflush(s_ptrace);
}
}  // namespace sly3
