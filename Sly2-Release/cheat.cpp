// See cheat.h. The original's cheat codes: the table (0x2BD4E0: 15 entries of 0x38 bytes {buttons, count, effect,
// argument, index, removed, the list link}; their buttons the six words before each), the matching (0x14BF48), the
// run (0x14BFF8) and the effects (0x14C140, 0x14C190, 0x14C1F0, 0x14C200, 0x14C2B0).
#include "cheat.h"

#include "audio.h"
#include "clock.h"
#include "joy.h"
#include "menu.h"
#include "ref.h"
#include "savedata.h"
#include "shop.h"
#include "sound.h"
#include "sw.h"
#include "task.h"
#include "transition.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace
{
// the PS2 pad's bits (the game's layout: libpad's two bytes swapped)
constexpr uint32_t L = 0x8000, D = 0x4000, R = 0x2000, U = 0x1000, R1 = 0x0008;

enum CHEATK { CK_Restart, CK_Episode, CK_Flag, CK_MegaJump, CK_Gadget };
enum FLAGK { FK_Headset, FK_AllEpisodes };
struct CHEAT
{
    const char* pchzName;
    uint32_t an[6];
    CHEATK ck;
    int arg;
    int i = 0;   // +0x10: the next button
};

// orig 0x2BD4E0.. (in the list's order, 0x14C2D8)
CHEAT s_acheat[] = {
    {"Restart current", {L, R1, U, D, U, L}, CK_Restart, 0},          // 0x14C140
    {"Tutorial", {R, L, U, U, U, R1}, CK_Episode, 0},                  // 0x14C190 (episode 0)
    {"Episode 1", {D, R1, L, R, R1, D}, CK_Episode, 1},
    {"Episode 2", {R1, L, R, R1, L, D}, CK_Episode, 2},
    {"Episode 3", {U, L, R, L, D, U}, CK_Episode, 3},
    {"Episode 4", {U, R, R, U, L, L}, CK_Episode, 4},
    {"Episode 5", {L, R1, D, D, U, R}, CK_Episode, 5},
    {"Episode 6", {D, U, R1, R1, L, D}, CK_Episode, 6},
    {"Episode 7", {L, L, L, D, D, R1}, CK_Episode, 7},
    {"Episode 8", {D, U, L, L, R1, R}, CK_Episode, 8},
    {"USB headset dialog", {L, R1, R1, D, D, L}, CK_Flag, FK_Headset},      // 0x14C1F0 on 0x3DD050
    {"All episodes and movies", {R, R1, D, R1, D, D}, CK_Flag, FK_AllEpisodes},  // 0x14C1F0 on 0x2BE26C
    {"Mega Jump", {R, L, R, L, R1, L}, CK_MegaJump, 0x1F},             // 0x14C200
    {"TOM", {L, L, D, R, L, R}, CK_Gadget, 0x29},                       // 0x14C2B0
    {"Time Rush", {D, D, U, D, R, L}, CK_Gadget, 0x2A},
};
constexpr int CCHEAT = int(sizeof s_acheat / sizeof s_acheat[0]);
constexpr int CBTN = 6;

float s_tRun = 0.0f;           // orig 0x2DDEC0 +0xC: the last press's time + 1 (0: nothing pending)
bool s_fAllEpisodes = false;   // orig 0x2BE26C
int s_icheatLast = -1;

bool FLog() { static const bool f = std::getenv("SLY2N_CHEATLOG") != nullptr; return f; }

// orig 0x001DAF38: the power-up n acquired (f) or not, and when acquired the task manager's +0xC and the event 0x3B
void SetItemAcquired(SW* psw, int n, bool f)
{
    if (FLog())
        std::printf("cheat: power-up %d acquired %d -> %d\n", n, [&] { const CRef r0 = RefGetProgress({"powerUps", "mppukfAcquired", std::to_string(n)}); return r0.m_tagk == TAGK_Bool ? int(r0.m_bool) : -1; }(), int(f));
    CRef r;
    r.SetBool(f);
    FSetProgress({"powerUps", "mppukfAcquired", std::to_string(n)}, r);
    if (f)
    {
        g_taskmgr.nItemC = n;
        if (psw) BroadcastTaskEvent(psw, 0x3B, nullptr);
    }
}

// orig 0x1FBB90 + 0x1FB2F8(0): a plain sound without a source
void StartPlainSound(int sfxid)
{
    if (CSndPlain* ps = PsndNewPlain(sfxid)) ps->SetSource(nullptr);
}

void RunCheat(SW* psw, const CHEAT& c)
{
    switch (c.ck)
    {
    case CK_Restart:
        // orig 0x0014C140: the master volume to 0 (the load sets it back), then the current world again at its
        // entrance (0x22B490: no record, point 393, no key, flags 0, no wipe)
        SetSoundMasterVolume(0.0f);
        RequestTransition(psw, nullptr, 0x189, -1, 0, -1);
        break;
    case CK_Episode:
    {
        // orig 0x0014C190: the episode's main world (0x210B98; none: episode 0's) at its entrance
        EnsureWorldRecords();
        const WREC* pw = PwrecEpisodeMain(c.arg);
        if (!pw) pw = PwrecEpisodeMain(0);
        RequestTransition(psw, pw ? pw->abRecord : nullptr, 0x189, -1, 0, -1);
        break;
    }
    case CK_Flag:
        // orig 0x0014C1F0: *arg = 1
        if (c.arg == FK_Headset)
        {
            std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
            g_sound.fEffect = 1;   // (0x3DD050: the dialog streams flagged for the USB headset, sound.cpp)
        }
        else s_fAllEpisodes = true;
        break;
    case CK_MegaJump:
        // orig 0x0014C200: with the game at 100% (0x1D82B8) the power-up 0x1F, else the sound 0x3E
        if (NGamePercent(g_gamesave) >= 100) SetItemAcquired(psw, c.arg, true);
        else StartPlainSound(0x3E);
        break;
    case CK_Gadget:
        // orig 0x0014C2B0: the power-up, then the players told (0x19F0A8)
        SetItemAcquired(psw, c.arg, true);
        NotifyPlayersPowerups(psw);
        break;
    }
}
}  // namespace

// orig 0x0014BF48 (p 0x2DDEC0, the pad, the old buttons): only the first pad (0x2DFC00), on a change to a non-zero
// set, with a menu up (0x3DA0B8)
void MatchCheatCodes(uint32_t grfbtnOld, uint32_t grfbtn)
{
    const uint32_t k = grfbtnOld & 0xFFFF;
    if (k == grfbtn || grfbtn == 0 || NMenuState() == 0) return;
    for (CHEAT& c : s_acheat)
    {
        if (c.i >= CBTN) continue;   // (complete: kept until the run)
        if (c.an[c.i] == grfbtn) ++c.i;
        else c.i = 0;
    }
    s_tRun = g_clock.t3C + 1.0f;
    if (FLog())
    {
        std::printf("cheat: press 0x%04X at %.3f:", grfbtn, g_clock.t3C);
        for (const CHEAT& c : s_acheat) std::printf(" %d", c.i);
        std::printf("\n");
    }
}

// orig 0x0014BFF8
void UpdateCheatCodes(SW* psw)
{
    if (s_tRun == 0.0f || g_clock.t3C < s_tRun) return;
    // (the entries' removal flag +0x14: none of the game's code sets it)
    CHEAT* pbest = nullptr;
    for (CHEAT& c : s_acheat)
        if (c.i >= CBTN && !pbest)   // (the longest, the first of equals: all are six long)
            pbest = &c;
    if (pbest)
    {
        s_icheatLast = int(pbest - s_acheat);
        if (FLog()) std::printf("cheat: \"%s\" at %.3f\n", pbest->pchzName, g_clock.t3C);
        StartPlainSound(0x34);   // (0x1FBB90 + 0x1FB2F8(0))
        RunCheat(psw, *pbest);
    }
    for (CHEAT& c : s_acheat) c.i = 0;   // (0x14BF18)
    s_tRun = 0.0f;
}

bool FCheatAllEpisodes() { return s_fAllEpisodes; }
int CCheatCodes() { return CCHEAT; }
CHEATDESC CheatCodeDesc(int i)
{
    const CHEAT& c = s_acheat[i];
    return {c.pchzName, c.an, c.ck == CK_MegaJump || c.ck == CK_Gadget};
}
void RunCheatCode(SW* psw, int i)
{
    if (i < 0 || i >= CCHEAT) return;
    s_icheatLast = i;
    StartPlainSound(0x34);
    RunCheat(psw, s_acheat[i]);
}
int IcheatLast() { return s_icheatLast; }
