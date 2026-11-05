// scptai.cpp: the SCPT commands' calls into the AI (kept apart: ai.h's CMD is another type than scpt.cpp's).
#include "ai.h"
#include "aijobs.h"
#include "guard.h"
#include "lo.h"
#include "brx.h"

#include <cstdio>
#include <cstdlib>

// orig: 0x00171CF8 / 0x00171D58 (a guard's reactions, its job 0x9C, told to run the command list (message 5) or to
// stop (message 1))
void TellGuardCommands(LO* pguard, LO* pcmds, bool fRun)
{
    GUARD* pg = pguard ? PguardFromLo(pguard) : nullptr;
    JOBREACT* pj = pg ? static_cast<JOBREACT*>(PjobFind(pg, 0x9C)) : nullptr;
    if (std::getenv("SLY2N_SCPTPRINT"))
        std::fprintf(stderr, "scpt: guard %d (cid 0x%X, guard %p, job 0x9C %p, inworld %d) commands %d (cid 0x%X)\n", pguard ? pguard->oid : -1,
                     pguard ? pguard->Cid() : 0, static_cast<void*>(pg), static_cast<void*>(pj), pguard ? pguard->fInWorld : 0,
                     pcmds ? pcmds->oid : -1, pcmds ? pcmds->Cid() : 0);
    if (std::getenv("SLY2N_SCPTPRINT"))
        if (CMDLIST* pl = dynamic_cast<CMDLIST*>(pcmds))
        {
            std::fprintf(stderr, "scpt: the command list %d:", pcmds->oid);
            for (auto& c : pl->acmd) std::fprintf(stderr, " op %d oid %d n8 %d;", c->op, c->oid, c->n8);
            std::fprintf(stderr, "\n");
        }
    if (!pj) return;
    AIMSG m;
    m.k = fRun ? 5 : 1;
    if (fRun) m.plo = pcmds;
    pj->React(m);  // 0x176750
}

// orig: 0x00171DA8: the guard's reactions (job 0x9C) still run that command list (message 5 at +0x30, its list +0x40)
bool FGuardRunsCommands(LO* pguard, LO* pcmds)
{
    GUARD* pg = pguard ? PguardFromLo(pguard) : nullptr;
    JOBREACT* pj = pg ? static_cast<JOBREACT*>(PjobFind(pg, 0x9C)) : nullptr;
    return pj && pj->msg.k == 5 && pj->msg.plo == pcmds;
}

// The guard class 0x3C (the one SCPT 1855 drives in job 1852): its update 0x1BA128 starts with the guards' base
// 0x207350 (guard.cpp); TODO(ai): the rest (its +0x2C0 bits 0x209A68, the carried object, 0x227C90)
#include "update.h"
namespace
{
void UpdateGuard3C(LO* plo)
{
    static int s_c = 0;
    if (std::getenv("SLY2N_SCPTPRINT") && s_c++ < 3)
        if (GUARD* pg = PguardFromLo(plo))
        {
            std::fprintf(stderr, "ai: guard 0x3C %d update: mode %d inworld %d jobs", plo->oid, pg->nMode, plo->fInWorld);
            for (auto& j : pg->stack.ajob) std::fprintf(stderr, " 0x%X", j->cid);
            std::fprintf(stderr, "\n");
        }
    if (PFNUPDATE pfn = PfnUpdateOrig(0x207350)) pfn(plo);
}
struct INITGUARD3C
{
    INITGUARD3C() { RegisterLoUpdate(0x1BA128, UpdateGuard3C); }
} s_initguard3c;
}  // namespace

// The command lists' loader methods (cid 0x71, EOPIDs 595..612: thunks 0x105D58..0x1065E0 -> 0x170B40..0x170DD0):
// each appends a command (0x170E00) with its arguments.
#include "objmethod.h"
void PostLoadCmdlist(CMDLIST* pl);  // ai.cpp: orig 0x170A58
namespace
{
CMD* Append(LO* plo, int op, OID oid)
{
    CMDLIST* pl = dynamic_cast<CMDLIST*>(plo);
    return pl ? PcmdAppend(pl, op, oid) : nullptr;
}
CRef Void() { return CRef(TAGK_Void); }
struct INITCMDLOADERS
{
    INITCMDLOADERS()
    {
        // orig: 0x00103DB0 (option 441) -> 0x00171CF8: start the authored command list.
        RegisterObjectMethod(0x103DB0, [](LO* p, const std::vector<CRef>& a) {
            TellGuardCommands(p, dynamic_cast<CMDLIST*>(PloArg(a, 0)), true);
            return Void();
        });
        // 0 the near object, 3 look at, 4 a message to, 8 / 9 / 10 (one object)
        RegisterObjectMethod(0x105D58, [](LO* p, const std::vector<CRef>& a) { Append(p, 0, NArg(a, 0)); return Void(); });
        RegisterObjectMethod(0x105F00, [](LO* p, const std::vector<CRef>& a) { Append(p, 3, NArg(a, 0)); return Void(); });
        RegisterObjectMethod(0x105F88, [](LO* p, const std::vector<CRef>& a) { Append(p, 4, NArg(a, 0)); return Void(); });
        RegisterObjectMethod(0x106208, [](LO* p, const std::vector<CRef>& a) { Append(p, 8, NArg(a, 0)); return Void(); });
        RegisterObjectMethod(0x106290, [](LO* p, const std::vector<CRef>& a) { Append(p, 9, NArg(a, 0)); return Void(); });
        RegisterObjectMethod(0x106318, [](LO* p, const std::vector<CRef>& a) { Append(p, 10, NArg(a, 0)); return Void(); });
        // 1 / 2: the ranges (an LM: +0x8 / +0xC)
        auto range = [](LO* p, int op, const std::vector<CRef>& a) {
            if (CMD* c = Append(p, op, -1); c && !a.empty())
            {
                const LM lm = a[0].GetLm();
                c->g8 = lm.gMin;
                c->gC = lm.gMax;
            }
        };
        static auto s_range = range;
        RegisterObjectMethod(0x105DE0, [](LO* p, const std::vector<CRef>& a) { s_range(p, 1, a); return Void(); });
        RegisterObjectMethod(0x105E70, [](LO* p, const std::vector<CRef>& a) { s_range(p, 2, a); return Void(); });
        // 5: the wait (+0x8 a float)
        RegisterObjectMethod(0x106010, [](LO* p, const std::vector<CRef>& a) {
            if (CMD* c = Append(p, 5, -1)) c->g8 = GArg(a, 0);
            return Void();
        });
        // 6: along a path (+0x8, +0xC)
        RegisterObjectMethod(0x106098, [](LO* p, const std::vector<CRef>& a) {
            if (CMD* c = Append(p, 6, NArg(a, 0)))
            {
                c->n8 = NArg(a, 1);
                c->nC = NArg(a, 2);
            }
            return Void();
        });
        // 7: go to (+0x10 an object, +0x14 a float)
        RegisterObjectMethod(0x106150, [](LO* p, const std::vector<CRef>& a) {
            if (CMD* c = Append(p, 7, NArg(a, 0)))
            {
                c->n10 = NArg(a, 1, -1);
                c->g14 = GArg(a, 2);
            }
            return Void();
        });
        // 11 / 12: lock / unlock
        RegisterObjectMethod(0x106448, [](LO* p, const std::vector<CRef>&) { Append(p, 11, -1); return Void(); });
        RegisterObjectMethod(0x1064C0, [](LO* p, const std::vector<CRef>&) { Append(p, 12, -1); return Void(); });
        // 13 / 14 / 15: a sub-job, an animation, an alarm (+0x8 the word)
        RegisterObjectMethod(0x1063A0, [](LO* p, const std::vector<CRef>& a) {
            if (CMD* c = Append(p, 13, NArg(a, 0))) c->n8 = NArg(a, 1);
            return Void();
        });
        RegisterObjectMethod(0x106538, [](LO* p, const std::vector<CRef>& a) {
            if (CMD* c = Append(p, 14, NArg(a, 0))) c->n8 = NArg(a, 1);
            return Void();
        });
        RegisterObjectMethod(0x1065E0, [](LO* p, const std::vector<CRef>& a) {
            if (CMD* c = Append(p, 15, NArg(a, 0))) c->n8 = NArg(a, 1);
            return Void();
        });
    }
} s_initcmdloaders;
}  // namespace
