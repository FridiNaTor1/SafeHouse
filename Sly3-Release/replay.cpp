// See replay.h. QoL, not the original game: the code here only reads the game's data and calls the game's own
// task start (0x1651D8, transition.cpp RequestTaskStart) and transitions.
#include "replay.h"

#include "clock.h"
#include "disc.h"
#include "frontend.h"
#include "hud.h"
#include "ref.h"
#include "savedata.h"
#include "sound.h"
#include "splayer.h"
#include "sw.h"
#include "task.h"
#include "transition.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

int IepEpMenuShown();   // frontend.cpp: the episode shown on the "Start Episode" screen (mode 8 sub -1), else -1
float DxTextUi(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88);   // splayer.cpp

namespace
{
bool FOff() { static const bool s_f = std::getenv("SLY2N_NOREPLAYJOBS") != nullptr; return s_f; }
bool FPrint() { static const bool s_f = std::getenv("SLY2N_REPLAYPRINT") != nullptr; return s_f; }

// the episodes' jobs (1..8), read once
bool s_fBuilt = false;
std::vector<JOBINFO> s_aajob[9];
int s_aiwrecMain[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};

// the list
struct MENU
{
    bool fShown = false;
    int iep = -1;
    std::vector<int> aijob;    // the open jobs (indices into s_aajob[iep])
    int isel = 0, itop = 0;
    bool fChosen = false;
    OID oidJobSel = -1;        // the cursor's job kept across a replay
} s_menu;
constexpr int CROW_SHOWN = 5;

// the replay
struct SESSION
{
    bool fActive = false;
    int iep = -1;
    JOBINFO job;
    GAMESAVE gsKeep;
} s_rep;
int s_iepReturn = -1;

// the text styles of the episode screen, taken from its own elements as they draw
TEXTDRAW s_tdTitle;
bool s_fTitle = false;

int NStateKey(const std::vector<int16_t>& akey)
{
    if (akey.empty()) return -1;
    const CRef r = RefGetProgressKey(akey);
    return r.m_tagk == TAGK_S32 ? r.m_n : -1;
}

// a job is offered once its first task was available (1) or done (2 / 3): the save has opened it
std::vector<int> AijobOpen(int iep)
{
    std::vector<int> a;
    if (iep < 1 || iep > 8) return a;
    for (size_t i = 0; i < s_aajob[iep].size(); ++i)
        if (NStateKey(s_aajob[iep][i].akeyFirst) >= 1) a.push_back(int(i));
    return a;
}

void PlaySound(int id) { PsndStartSound(id); }   // (the menus' 0x1FBB90 + 0x1FB2F8)

void Text(const TEXTDRAW& st, const std::string& str, float x, float y, float size, float wBox, const glm::vec4& off,
          const glm::vec4& sc)
{
    if (!st.pfont) return;
    g_hud.DrawTextBox(*st.pfont, st.pfontIcons, str, x, y, size, st.sx, st.sy, wBox, 0.0f, 0, 0, off, sc, st.offIcon, st.depth,
                      wBox > 0.0f);
}

bool FTextHook(const TEXTDRAW& td);
}  // namespace

void BuildReplayJobTables(int iwrecLoading)
{
    if (s_fBuilt || FOff() || iwrecLoading != 1) return;
    s_fBuilt = true;
    g_pfnTextDrawHook = FTextHook;
    const std::vector<WREC>& awrec = AwrecWorlds();
    for (int iep = 1; iep <= 8; ++iep)
    {
        const WREC* pw = PwrecEpisodeMain(iep);
        if (!pw) continue;
        const int iwrec = int(pw - awrec.data());
        s_aiwrecMain[iep] = iwrec;
        auto psw = std::make_unique<SW>();   // (as the transition makes the world it loads)
        psw->filter = iwrec;
        std::size_t cbRead = 0, cbFile = 0;
        if (!pw->fk.cb || !FLoadWorldFile(psw.get(), pw->fk, "", &cbRead, &cbFile)) continue;
        s_aajob[iep] = AjobinfoOfLoadedWorld(psw.get());
        for (JOBINFO& j : s_aajob[iep])
            if (j.iwrecFirst < 0) j.iwrecFirst = iwrec;
        if (FPrint())
            for (const JOBINFO& j : s_aajob[iep])
                std::printf("replay: episode %d (world %d) job %d '%s' first %d (world %d) last %d tasks %zu: %s\n", iep, iwrec, j.oidJob,
                            j.strName.c_str(), j.oidTaskFirst, j.iwrecFirst, j.oidTaskLast, j.aoidTask.size(), j.strDesc.c_str());
    }
}

void OnReplayWorldLoaded(int iwrec, int grfReq)
{
    if (!s_rep.fActive) return;
    if (iwrec != 1 && !(grfReq & 1)) return;   // (the front end, or the title)
    MergeProgressKeepDone(g_gamesave, s_rep.gsKeep);
    if (iwrec == 1) s_iepReturn = s_rep.iep;
    if (FPrint()) std::printf("replay: ended (world %d), progress kept\n", iwrec);
    s_rep.fActive = false;
}

int NUpdateReplayMenu(int iep)
{
    if (FOff() || !s_fBuilt) return 0;
    MENU& m = s_menu;
    if (!m.fShown)
    {
        m.fChosen = false;
        const std::vector<int> a = AijobOpen(iep);
        if (a.empty()) return 0;
        if (GrfbtnTakePressed(0x20)) return 2;   // (CIRCLE: back, TRIANGLE being the list)
        if (!GrfbtnTakePressed(0x10)) return 0;
        m = MENU{};
        m.fShown = true;
        m.iep = iep;
        m.aijob = a;
        m.oidJobSel = s_menu.oidJobSel;
        for (size_t i = 0; i < a.size(); ++i)
            if (s_aajob[iep][size_t(a[i])].oidJob == s_menu.oidJobSel) m.isel = int(i);
        m.itop = std::max(0, m.isel - CROW_SHOWN + 1);
        ClearJoySteps();
        PlaySound(0x34);
        return 1;
    }
    if (m.iep != iep) { CloseReplayMenu(); return 0; }
    if (GrfbtnTakePressed(0x10 | 0x20))
    {
        PlaySound(0x35);
        CloseReplayMenu();
        return 1;
    }
    if (GrfbtnTakePressed(0x40))
    {
        PlaySound(0x34);
        m.fChosen = true;
        m.oidJobSel = s_aajob[iep][size_t(m.aijob[size_t(m.isel)])].oidJob;
        return 3;
    }
    const int n = NStepUpDown();
    if (n != 0)
    {
        const int i = m.isel + (n < 0 ? -1 : 1);
        if (i < 0 || i >= int(m.aijob.size())) PlaySound(0x36);
        else
        {
            m.isel = i;
            if (m.isel < m.itop) m.itop = m.isel;
            if (m.isel >= m.itop + CROW_SHOWN) m.itop = m.isel - CROW_SHOWN + 1;
            PlaySound(0x35);
        }
    }
    return 1;
}

bool FReplayChosen() { return s_menu.fShown && s_menu.fChosen; }

void CloseReplayMenu()
{
    const OID oid = s_menu.oidJobSel;
    s_menu = MENU{};
    s_menu.oidJobSel = oid;
}

void StartReplay(SW* psw)
{
    MENU& m = s_menu;
    if (!FReplayChosen()) return;
    const JOBINFO& j = s_aajob[m.iep][size_t(m.aijob[size_t(m.isel)])];
    s_rep.fActive = true;
    s_rep.iep = m.iep;
    s_rep.job = j;
    s_rep.gsKeep = g_gamesave;
    if (FPrint()) std::printf("replay: job %d '%s' (episode %d): task %d in world %d\n", j.oidJob, j.strName.c_str(), m.iep, j.oidTaskFirst, j.iwrecFirst);
    CloseReplayMenu();
    // orig 0x001651D8: not in this world: queued, its world loaded (request flags 8)
    RequestTaskStart(psw, j.iwrecFirst, j.oidTaskFirst);
}

int IepReplayReturn()
{
    const int iep = s_iepReturn;
    s_iepReturn = -1;
    return iep;
}

bool FReplayActive() { return s_rep.fActive; }

void MergeReplaySave(GAMESAVE& gs)
{
    if (s_rep.fActive) MergeProgressKeepDone(gs, s_rep.gsKeep);
}

bool FReplayJobEnd(SW* psw, JOB* pjob)
{
    if (!s_rep.fActive || !pjob || pjob->oid != s_rep.job.oidJob) return false;
    if (FPrint()) std::printf("replay: job %d ended (t %.2f): back to the episode menu\n", pjob->oid, g_clock.t);
    // as the pause menu's Exit to Episode Menu (0x1B09A8)
    RequestTransition(psw, g_disc.PbElf(0x2A3928, 64), 0x189, -1, 0x80, -1);
    return true;
}

namespace
{
// the episode screen's text elements: the line under them (the list closed) or the list in their place
bool FTextHook(const TEXTDRAW& td)
{
    const int iep = IepEpMenuShown();
    if (iep < 1 || td.kind < 0) return false;
    if (td.kind == 0 && td.id == 0x6D + iep)   // the episode's name (0x6D + episode, as the job help's 0x51B): the selected row's colours
    {
        s_tdTitle = td;
        s_fTitle = true;
        return false;
    }
    const bool fStart = td.kind == 0 && td.id == 0x6C, fPercent = td.kind == 1 && td.id == 0x52F, fTime = td.kind == 1 && td.id == 0x530;
    if (!fStart && !fPercent && !fTime) return false;
    const MENU& m = s_menu;
    if (!m.fShown || m.iep != iep)
    {
        // "(TRIANGLE): Replay Jobs" one line under the time, in the "(X): Start Episode" line's style
        if (fTime && !AijobOpen(iep).empty())
            Text(td, "&2T&.: Replay Jobs", td.x, td.y + 35.0f, td.size, 0.0f, td.off, td.sc);
        return false;
    }
    if (!fStart) return true;
    // the rows (the selected one in the episode name's colours), the description, the buttons
    const std::vector<JOBINFO>& ajob = s_aajob[iep];
    const float size = td.size * 0.8f, dy = 28.0f;
    const float y0 = td.y - 50.0f;
    const glm::vec4 offSel = s_fTitle ? s_tdTitle.off : td.off, scSel = s_fTitle ? s_tdTitle.sc : td.sc;
    const int cRow = std::min<int>(CROW_SHOWN, int(m.aijob.size()));
    for (int r = 0; r < cRow; ++r)
    {
        const int i = m.itop + r;
        if (i >= int(m.aijob.size())) break;
        const JOBINFO& j = ajob[size_t(m.aijob[size_t(i)])];
        const bool fSel = i == m.isel;
        Text(td, j.strName, td.x + 22.0f, y0 + dy * float(r), size, 0.0f, fSel ? offSel : td.off, fSel ? scSel : td.sc);
    }
    const JOBINFO& jSel = ajob[size_t(m.aijob[size_t(m.isel)])];
    const float yDesc = y0 + dy * float(CROW_SHOWN) + 6.0f;
    Text(td, jSel.strDesc, td.x + 22.0f, yDesc, td.size * 0.55f, 500.0f, td.off, td.sc);
    const std::string strHint = "&2X&.: Replay Job    &2T&.: Back";
    Text(td, strHint, td.x, yDesc + 44.0f, td.size * 0.7f, 0.0f, td.off, td.sc);
    // (the icon font has no arrows: the position in the list after the buttons when it scrolls)
    if (int(m.aijob.size()) > CROW_SHOWN && td.pfont)
    {
        const std::string str = std::to_string(m.isel + 1) + " / " + std::to_string(m.aijob.size());
        const float dx = DxTextUi(*td.pfont, td.pfontIcons, strHint, td.size * 0.7f / 51.2f) * td.sx;
        Text(td, str, td.x + dx + 40.0f, yDesc + 44.0f, td.size * 0.7f, 0.0f, td.off, td.sc);
    }
    return true;
}
}  // namespace
