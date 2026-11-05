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
        // 7: go to (+0x10 an object, +0x14 a float). The splice's arguments are (oid, float, oid) (OTYP 6 / 1 / 6) and
        // 0x170C90 stores the float at +0x14 and the third argument at +0x10 (the object faced on the way: JOBGOTO's
        // +0x40, move kind 8). R42: read the other way round, the Ep4 Contessa's chase legs (77370: (pt 50 9)) had no
        // face object (Murray, 9), so she walked forward instead of running backwards facing him.
        RegisterObjectMethod(0x106150, [](LO* p, const std::vector<CRef>& a) {
            if (CMD* c = Append(p, 7, NArg(a, 0)))
            {
                c->g14 = GArg(a, 1);
                c->n10 = NArg(a, 2, -1);
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
