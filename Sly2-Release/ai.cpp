// The command lists (cid 0x71, module 0x1709A8). See ai.h. Reference: Sly2-Reference/Sly2-Release/game/m_1709A8.cpp.

#include "ai.h"
#include "sound.h"
#include <cstdlib>
#include <cstdio>
#include "clock.h"
#include "joy.h"

#include "animfind.h"
#include "brx.h"
#include "sw.h"

#include <algorithm>

CMD* CMDLIST::PcmdNext(const CMD* p) const
{
    for (size_t i = 0; i + 1 < acmd.size(); ++i)
        if (acmd[i].get() == p) return acmd[i + 1].get();
    return nullptr;
}

LO* PloNewCmdlist() { return new CMDLIST(); }

// orig: 0x00170E00
CMD* PcmdAppend(CMDLIST* pl, int op, OID oid)
{
    auto p = std::make_unique<CMD>();
    p->op = op;
    p->oid = oid;
    CMD* pr = p.get();
    pl->acmd.push_back(std::move(p));
    return pr;
}

// orig: 0x00170A58
void PostLoadCmdlist(CMDLIST* pl)
{
    LO* ploCtx = pl;
    for (const auto& p : pl->acmd)
    {
        if (p->op == 0) ploCtx = ploCtx ? PloFindSwNearest(ploCtx, p->oid) : PloFindSwAny(p->oid);
        else if (p->op == 6)
        {
            LO* po = ploCtx ? PloFindSwNearest(ploCtx, p->oid) : nullptr;
            // (a path, not a polygon (classes 0xB / 0xD): its AI record 0x1E9C30; TODO(ai))
            (void)po;
        }
    }
}

GAMEPAUSE g_gamepause;

// orig: 0x0020EFC0
void PauseGame(int grf)
{
    if (++g_gamepause.c78 == 1) LockJoy(2);  // 0x187078
    if (grf & 1 && ++g_gamepause.c80 == 1) ShowSubtitleBox();  // (0x20F128: at 1 0x1A3078)
    if (grf & 2) ++g_gamepause.c84;  // (0x20F198)
    if (std::getenv("SLY2N_PAUSEPRINT")) std::printf("pause game grf %d t %.3f: %d %d %d\n", grf, g_clock.t, g_gamepause.c78, g_gamepause.c80, g_gamepause.c84);
}

// orig: 0x0020F030
void ResumeGame(int grf)
{
    if (--g_gamepause.c78 == 0)
    {
        UnlockJoy(2);                      // 0x1870A0
        ClearJoyBtns(&g_joyps2, 8);  // 0x187E08 (the game layout's 8: R1, the binocucom)
    }
    if (grf & 1 && --g_gamepause.c80 == 0) HideSubtitleBox();  // (0x20F160: at 0, 0x1A30D0(0x2E1E60))
    if (grf & 2) --g_gamepause.c84;  // (0x20F1A8)
    if (std::getenv("SLY2N_PAUSEPRINT")) std::printf("resume game grf %d t %.3f: %d %d %d\n", grf, g_clock.t, g_gamepause.c78, g_gamepause.c80, g_gamepause.c84);
}

// orig: 0x0020F0C0
void PauseTalk()
{
    if (++g_gamepause.c7C == 1) LockJoy(4);  // 0x187078
}

// orig: 0x0020F0F0
void ResumeTalk()
{
    if (--g_gamepause.c7C == 0) UnlockJoy(4);  // 0x1870A0
}

