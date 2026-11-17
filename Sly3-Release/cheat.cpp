// See cheat.h. Sly 3's cheat codes: the table (0x3440E0: 34 entries {buttons, count, effect, argument, index, removed,
// the list link}), the matching (0x163F58), the run (0x164028) and the effects (0x164170 an episode, 0x1641A0 a flag,
// 0x1641B0 the dogfight's plane, 0x164228 a job again).
#include "cheat.h"
#include "audio.h"
#include "clock.h"
#include "epmenu3.h"
#include "sound.h"
#include "sw.h"
#include "transition.h"
#include "game/cond3.h"
#include "game/task3.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

void RequestTaskStartThere(SW* psw, int iwrec, OID oidTask);   // transition.cpp (a task of another world, by its load)

namespace
{
// the PS2 pad's bits (the game's layout: libpad's two bytes swapped)
constexpr uint32_t L = 0x8000, D = 0x4000, R = 0x2000, U = 0x1000, R1 = 0x0008, L1 = 0x0004, R2 = 0x0002, L2 = 0x0001;

enum CHEATK { CK_Episode, CK_Headset, CK_AllEpisodes, CK_Dogfight, CK_Job };

struct CHEAT
{
    const char* pchzName;
    uint32_t an[6];
    CHEATK ck;
    int arg;
    int i = 0;   // +0x10: the next button
};

// orig3 0x3440E0.. (in the list's order, 0x164518)
CHEAT s_acheat[] = {
    {"Prologue", {R1, D, R, U, U, D}, CK_Episode, 1},                 // 0x164170 -> 0x289198(episode)
    {"Episode 1 (Venice)", {R1, R, D, R, R1, R}, CK_Episode, 3},
    {"Episode 2 (Outback)", {U, D, L, R1, R, L}, CK_Episode, 4},
    {"Episode 3 (Holland)", {R1, R, R1, R, R, R1}, CK_Episode, 5},
    {"Episode 4 (China)", {U, D, D, D, R1, L}, CK_Episode, 6},
    {"Episode 5 (Pirate)", {R1, R1, U, R, R1, R}, CK_Episode, 7},
    {"Episode 6 (Kaine Island)", {L, D, R1, L, R1, U}, CK_Episode, 8},
    {"USB headset dialog", {R, U, R1, R, U, D}, CK_Headset, 0},       // 0x1641A0 on 0x475000
    {"Every episode open", {R1, U, U, R1, R1, D}, CK_AllEpisodes, 0}, // 0x1641A0 on 0x3448E0
    {"Dogfight: the plane's message 5", {R1, R1, R, D, D, R}, CK_Dogfight, 0},   // 0x1641B0
    // 0x164228: a job done before, again (its task's world at its checkpoint)
    {"Job 1825", {L, R2, R, L1, R2, L1}, CK_Job, 0x721},
    {"Job 1826", {D, L2, U, L, R2, L2}, CK_Job, 0x722},
    {"Job 1827", {D, L2, R1, L2, R1, R}, CK_Job, 0x723},
    {"Job 1828", {R, L2, L, U, R, D}, CK_Job, 0x724},
    {"Job 1829", {D, U, R1, U, R2, L2}, CK_Job, 0x725},
    {"Job 1830", {L2, U, L, L1, L2, L1}, CK_Job, 0x726},
    {"Job 1831", {R2, R1, L1, L, L1, D}, CK_Job, 0x727},
    {"Job 1832", {L2, R1, R2, L2, L1, U}, CK_Job, 0x728},
    {"Job 1833", {D, R, L, L2, R1, R}, CK_Job, 0x729},
    {"Job 1834", {L, R, L1, R2, R, R2}, CK_Job, 0x72A},
    {"Job 1835", {L2, L, R1, L2, L1, D}, CK_Job, 0x72B},
    {"Job 1836", {L1, L, L2, L, U, L1}, CK_Job, 0x72C},
    {"Job 1837", {L2, R2, R, D, L1, R2}, CK_Job, 0x72D},
    {"Job 1838", {L, R2, R, U, L1, R2}, CK_Job, 0x72E},
    {"Job 1839", {R2, R1, L1, R1, R2, R1}, CK_Job, 0x72F},
    {"Job 1840", {R, L, U, L, R2, L}, CK_Job, 0x730},
    {"Job 1841", {L, L2, L1, L2, R2, R}, CK_Job, 0x731},
    {"Job 1842", {L2, D, R1, U, L, D}, CK_Job, 0x732},
    {"Job 1843", {L1, R, R2, U, R, L}, CK_Job, 0x733},
    {"Job 1844", {R1, L2, D, R2, R, U}, CK_Job, 0x734},
    {"Job 1845", {R, D, L2, R, R2, D}, CK_Job, 0x735},
    {"Job 1846", {R, R2, U, R, U, D}, CK_Job, 0x736},
    {"Job 1847", {L, U, L, U, L1, L2}, CK_Job, 0x737},
    {"Job 1848", {L1, R2, L1, D, L1, D}, CK_Job, 0x738},
};
constexpr int CCHEAT = int(sizeof s_acheat / sizeof s_acheat[0]);
constexpr int CBTN = 6;

float s_tRun = 0.0f;           // orig3 the list's +0xC: the last press's time + 1 (0: nothing pending)
bool s_fAllEpisodes = false;   // orig3 0x3448E0
int s_icheatLast = -1;

bool FLog() { static const bool f = std::getenv("SLY2N_CHEATLOG") != nullptr; return f; }

// orig3 0x244F08(0x2459F8(sfx), 0): a plain sound without a source
void StartPlainSound(int sfxid)
{
    if (CSndPlain* ps = PsndNewPlain(sfxid)) ps->SetSource(nullptr);
}

void RunCheat(SW* psw, const CHEAT& c)
{
    switch (c.ck)
    {
    case CK_Episode:
        // orig3 0x00164170: the episode menu's start of the episode (0x289198)
        StartEpisode3(psw, c.arg);
        break;
    case CK_Headset:
    {
        // orig3 0x001641A0 on 0x475000: the dialog streams flagged for the USB headset (sound.cpp)
        std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
        g_sound.fEffect = 1;
        break;
    }
    case CK_AllEpisodes: s_fAllEpisodes = true; break;   // orig3 0x001641A0 on 0x3448E0
    case CK_Dogfight:
        // orig3 0x001641B0: in h_dogfight (world 21) the first pad's player, when it is of class 0x52, gets the message 5
        // (its vt slot 3). TODO(sly3): the plane (class 0x52) is not ported yet: logged only.
        if (g_nFilterCur3 == 21) std::printf("cheat: the dogfight's message 5 (class 0x52 not ported)\n");
        break;
    case CK_Job:
    {
        // orig3 0x00164228: the job's task (found in the episode menu's world) must have been done (state >= 2), else
        // the sound 0x7E (none found: 0x6A); then its checkpoint and world are set and the game goes there (0x289070)
        TASK3* ptask = PtaskFind3(c.arg);
        if (!ptask) { StartPlainSound(0x6A); break; }
        if (ptask->nState < 2) { StartPlainSound(0x7E); break; }
        RequestTaskStartThere(psw, ptask->iworld, c.arg);
        break;
    }
    }
}
}  // namespace

// orig3 0x00163F58 (the list 0x36BB88, the pad): only in the episode menu's world (0x25E150() == 0; 0x46F74C is never
// set), each newly pressed set of buttons
void MatchCheatCodes(uint32_t grfbtnOld, uint32_t grfbtn)
{
    if (g_nFilterCur3 != 0) return;
    const uint32_t btn = grfbtn & ~grfbtnOld & 0xFFFF;
    if (!btn) return;
    for (CHEAT& c : s_acheat)
    {
        if (c.i >= CBTN) continue;   // (complete: kept until the run)
        if (c.an[c.i] == btn) ++c.i;
        else c.i = 0;
    }
    s_tRun = g_clock.t3C + 1.0f;
    if (FLog())
    {
        std::printf("cheat: press 0x%04X at %.3f:", btn, g_clock.t3C);
        for (const CHEAT& c : s_acheat) std::printf(" %d", c.i);
        std::printf("\n");
    }
}

// orig3 0x00164028
void UpdateCheatCodes(SW* psw)
{
    if (s_tRun == 0.0f || g_clock.t3C < s_tRun) return;
    CHEAT* pbest = nullptr;
    for (CHEAT& c : s_acheat)
        if (c.i >= CBTN && !pbest)   // (the longest, the first of equals: all are six long)
            pbest = &c;
    if (pbest)
    {
        s_icheatLast = int(pbest - s_acheat);
        if (FLog()) std::printf("cheat: \"%s\" at %.3f\n", pbest->pchzName, g_clock.t3C);
        StartPlainSound(0x34);
        RunCheat(psw, *pbest);
    }
    for (CHEAT& c : s_acheat) c.i = 0;   // (0x163F28)
    s_tRun = 0.0f;
}

bool FCheatAllEpisodes() { return s_fAllEpisodes; }
int CCheatCodes() { return CCHEAT; }
CHEATDESC CheatCodeDesc(int i)
{
    const CHEAT& c = s_acheat[i];
    return {c.pchzName, c.an, false};
}
void RunCheatCode(SW* psw, int i)
{
    if (i < 0 || i >= CCHEAT) return;
    s_icheatLast = i;
    StartPlainSound(0x34);
    RunCheat(psw, s_acheat[i]);
}
int IcheatLast() { return s_icheatLast; }
