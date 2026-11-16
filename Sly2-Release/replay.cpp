// See replay.h. QoL, not the original game: the code here only reads the game's data and calls the game's own
// task start (0x1651D8, transition.cpp RequestTaskStart) and transitions.
#include "replay.h"
#include "promptglyph.h"
#include "region.h"
#include "porttext.h"

#include "clock.h"
#include "disc.h"
#include "frontend.h"
#include "hud.h"
#include "joy.h"
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
int s_nLangBuilt = -1;   // (the language of the names: built again in another one, the title's language menu)
std::vector<JOBINFO> s_aajob[9];
int s_aiwrecMain[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};

// the multi-part operations: an operation's jobs the game chains directly (each part's last task's next task is the
// next part's first task: JobCompleteNext, no return to the safe house). Found in the episodes' job exits (R57): every
// "Operation:" job (the game's strings hold six) is its episode's last job; three of them run straight into the boss
// fight, a job of its own. Operation: Trojan Tank, High Road and "Choo-Choo" are single jobs (their last task is the
// episode's end), so they need no group. Ep2's Operation: Hippo Drop has no job or string of its own (a wiki name for
// three chained jobs): its name is ours. Ep8's last jobs (Mega-Jump Job -> Carmelita's Gunner -> Showdown with
// Clock-La) chain too, but no "Operation:" name exists for them. The parts stay listed under the operation.
struct OPERATION
{
    int iep;
    PTX ptxName;                // its name in the game's language (porttext.h); PTX_Max: the first part's (the game's
                                // "Operation:" job)
    std::vector<OID> aoidJob;   // the parts in play order
};
const OPERATION s_aop[] = {
    {1, PTX_Max, {1889, 1915}},              // Operation: Thunder Beak -> Printing Press Duel
    {2, PTX_OpHippoDrop, {2089, 2116, 2119}},  // Bomb the Bridge -> Tango with Carmelita -> Clear the way for Murray
    {3, PTX_Max, {2296, 2324}},              // Operation: Wet Tiger -> Showdown with Rajan
    {7, PTX_Max, {3191, 3274}},              // Operation: Canada Games -> Brains over Brawn
};
// (built: the operations whose parts were all found, chained in the data)
struct OPINFO
{
    int iop = -1;
    std::vector<int> aijob;     // the parts (indices into s_aajob[iep])
    std::string strName, strDesc;
};
std::vector<OPINFO> s_aaop[9];

// the list: a row is a job, or an operation (the whole operation replayed in sequence)
struct ROW
{
    int ijob = -1;              // a job (index into s_aajob[iep])
    int iop = -1;               // else an operation (index into s_aaop[iep])
    int ipart = -1;             // a job listed under its operation: its place in it (drawn indented, numbered)
};
struct MENU
{
    bool fShown = false;
    int iep = -1;
    std::vector<ROW> arow;      // the open jobs and operations
    int isel = 0, itop = 0;
    bool fChosen = false;
    OID oidJobSel = -1;        // the cursor's job kept across a replay (an operation: -2 - its index)
} s_menu;
constexpr int CROW_SHOWN = 5;

// the replay
struct SESSION
{
    bool fActive = false;
    int iep = -1;
    JOBINFO job;                // the job playing
    std::vector<JOBINFO> ajobNext;   // an operation: its parts still to play
    GAMESAVE gsKeep;
} s_rep;
int s_iepReturn = -1;

// the text styles of the episode screen, taken from its own elements as they draw
TEXTDRAW s_tdTitle;
bool s_fTitle = false;
// each text line is two elements, a black shadow (2 units lower) and the text: the selected row takes the name's text
// colours, its shadow stays black
bool FShadow(const TEXTDRAW& td) { return td.off.r + td.off.g + td.off.b < 24.0f; }
// the episode name's lines as wrapped (a two-line name takes the first row's place); the most seen for the episode, as
// its passes (the shadow, the text) are drawn with different boxes
int s_aclineTitle[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};

// the rows shown at once: one fewer per extra line of the episode name above them
int CrowShown(int iep) { return std::max(1, CROW_SHOWN - (s_aclineTitle[std::clamp(iep, 0, 8)] - 1)); }

int NStateKey(const std::vector<int16_t>& akey)
{
    if (akey.empty()) return -1;
    const CRef r = RefGetProgressKey(akey);
    return r.m_tagk == TAGK_S32 ? r.m_n : -1;
}

// a job is offered once its first task was available (1) or done (2 / 3): the save has opened it; an operation once
// all its parts are (listed before its first part, its parts under it, numbered)
bool FJobOpen(int iep, int ijob) { return NStateKey(s_aajob[iep][size_t(ijob)].akeyFirst) >= 1; }
std::vector<ROW> ArowOpen(int iep)
{
    std::vector<ROW> a;
    if (iep < 1 || iep > 8) return a;
    for (size_t i = 0; i < s_aajob[iep].size(); ++i)
    {
        if (!FJobOpen(iep, int(i))) continue;
        int ipart = -1;
        for (size_t k = 0; k < s_aaop[iep].size(); ++k)
        {
            const OPINFO& op = s_aaop[iep][k];
            const auto it = std::find(op.aijob.begin(), op.aijob.end(), int(i));
            if (it == op.aijob.end()) continue;
            if (!std::all_of(op.aijob.begin(), op.aijob.end(), [&](int ij) { return FJobOpen(iep, ij); })) continue;
            ipart = int(it - op.aijob.begin());
            if (ipart == 0) a.push_back(ROW{-1, int(k), -1});
        }
        a.push_back(ROW{int(i), -1, ipart});
    }
    static bool s_fPrinted = false;
    if (FPrint() && !s_fPrinted)
    {
        s_fPrinted = true;
        for (int iepP = 1; iepP <= 8; ++iepP)
            for (const JOBINFO& j : s_aajob[iepP])
                std::printf("replay: episode %d job %d '%s' first-task %d state %d last-task %d state %d\n", iepP, j.oidJob, j.strName.c_str(),
                            j.oidTaskFirst, NStateKey(j.akeyFirst), j.oidTaskLast, NStateKey(j.akeyLast));
    }
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

OID OidRow(int iep, const ROW& row) { return row.iop >= 0 ? -2 - row.iop : s_aajob[iep][size_t(row.ijob)].oidJob; }

bool FTextHook(const TEXTDRAW& td);

// The list's back button: the region's (region.h BtnMenuBack), but CIRCLE where that is TRIANGLE, which opens and
// closes the list (USA / Europe: confirm CROSS, back CIRCLE; Japan: confirm CIRCLE, back CROSS, as its menus)
uint32_t BtnReplayBack() { return BtnMenuBack() == 0x10 ? 0x20 : BtnMenuBack(); }
}  // namespace

void BuildReplayJobTables(int iwrecLoading)
{
    if ((s_fBuilt && s_nLangBuilt == g_nLang) || FOff() || iwrecLoading != 1) return;
    s_fBuilt = true;
    s_nLangBuilt = g_nLang;
    for (auto& a : s_aajob) a.clear();
    for (auto& a : s_aaop) a.clear();
    g_pfnTextDrawHook = FTextHook;
    const auto grfjoyLoading = g_grfjoyLock;
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
        if (pw->fk.cb && FLoadWorldFile(psw.get(), pw->fk, "", &cbRead, &cbFile))
        {
            s_aajob[iep] = AjobinfoOfLoadedWorld(psw.get());
            for (JOBINFO& j : s_aajob[iep])
                if (j.iwrecFirst < 0) j.iwrecFirst = iwrec;
            if (FPrint())
                for (const JOBINFO& j : s_aajob[iep])
                {
                    std::printf("replay: episode %d (world %d) job %d '%s' first %d (world %d) last %d next %d tasks %zu:", iep, iwrec, j.oidJob,
                                j.strName.c_str(), j.oidTaskFirst, j.iwrecFirst, j.oidTaskLast, j.oidTaskNext, j.aoidTask.size());
                    for (OID oid : j.aoidTask) std::printf(" %d", oid);
                    std::printf(": %s\n", j.strDesc.c_str());
                }
        }
        // Loading metadata also registers ambient objects, scripts and resident streams.
        // Use the ordinary world teardown (20E068 / sound reset 1FD700) before
        // releasing its objects; the copied job descriptions need none of that state.
        UnloadWorld(psw, false);
        // the operations of this episode: every part found, each part's last task followed by the next part's first
        for (size_t k = 0; k < std::size(s_aop); ++k)
        {
            const OPERATION& op = s_aop[k];
            if (op.iep != iep) continue;
            OPINFO oi;
            oi.iop = int(k);
            for (OID oid : op.aoidJob)
                for (size_t i = 0; i < s_aajob[iep].size(); ++i)
                    if (s_aajob[iep][i].oidJob == oid) oi.aijob.push_back(int(i));
            bool fChained = oi.aijob.size() == op.aoidJob.size();
            for (size_t n = 0; fChained && n + 1 < oi.aijob.size(); ++n)
                fChained = s_aajob[iep][size_t(oi.aijob[n])].oidTaskNext == s_aajob[iep][size_t(oi.aijob[n + 1])].oidTaskFirst;
            if (!fChained)
            {
                if (FPrint()) std::printf("replay: operation '%s' not found chained in episode %d: not offered\n", op.ptxName != PTX_Max ? StrPort(op.ptxName).c_str() : "(its first part's)", iep);
                continue;
            }
            oi.strName = op.ptxName != PTX_Max ? StrPort(op.ptxName) : s_aajob[iep][size_t(oi.aijob.front())].strName;
            oi.strDesc = StrPort(PTX_ReplayOperation);
            for (size_t n = 0; n < oi.aijob.size(); ++n)
                oi.strDesc += StrPort(n ? PTX_ListSep : PTX_ListFirst) + s_aajob[iep][size_t(oi.aijob[n])].strName;
            oi.strDesc += StrPort(PTX_ListEnd);
            s_aaop[iep].push_back(std::move(oi));
        }
    }
    if (FPrint())
        for (int iep = 1; iep <= 8; ++iep)
            for (const JOBINFO& j : s_aajob[iep])
                std::printf("replay: episode %d job %d '%s' first-task state %d last-task state %d\n", iep, j.oidJob, j.strName.c_str(),
                            NStateKey(j.akeyFirst), NStateKey(j.akeyLast));
    // The caller is still loading the real frontend. Temporary-world teardown
    // clears pad locks, so retain the caller's lock until frontend startup ends it.
    g_grfjoyLock = grfjoyLoading;
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
        const std::vector<ROW> a = ArowOpen(iep);
        if (a.empty()) return 0;
        if (GrfbtnTakePressed(BtnReplayBack())) return 2;   // (USA / Europe: CIRCLE back, TRIANGLE being the list)
        if (!GrfbtnTakePressed(0x10)) return 0;
        m = MENU{};
        m.fShown = true;
        m.iep = iep;
        m.arow = a;
        m.oidJobSel = s_menu.oidJobSel;
        for (size_t i = 0; i < a.size(); ++i)
            if (OidRow(iep, a[i]) == s_menu.oidJobSel) m.isel = int(i);
        m.itop = std::max(0, m.isel - CrowShown(m.iep) + 1);
        ClearJoySteps();
        PlaySound(0x34);
        return 1;
    }
    if (m.iep != iep) { CloseReplayMenu(); return 0; }
    if (GrfbtnTakePressed(0x10 | BtnReplayBack()))
    {
        PlaySound(0x35);
        CloseReplayMenu();
        return 1;
    }
    if (GrfbtnTakePressed(BtnMenuConfirm()))
    {
        PlaySound(0x34);
        m.fChosen = true;
        m.oidJobSel = OidRow(iep, m.arow[size_t(m.isel)]);
        return 3;
    }
    const int n = NStepUpDown();
    if (n != 0)
    {
        const int i = m.isel + (n < 0 ? -1 : 1);
        if (i < 0 || i >= int(m.arow.size())) PlaySound(0x36);
        else
        {
            m.isel = i;
            if (m.isel < m.itop) m.itop = m.isel;
            if (m.isel >= m.itop + CrowShown(m.iep)) m.itop = m.isel - CrowShown(m.iep) + 1;
            PlaySound(0x35);
        }
    }
    return 1;
}

bool FReplayOffered(int iep) { return !FOff() && s_fBuilt && !ArowOpen(iep).empty(); }
bool FReplayListShown() { return s_menu.fShown; }
uint32_t BtnReplayBackButton() { return BtnReplayBack(); }

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
    const ROW& row = m.arow[size_t(m.isel)];
    std::vector<JOBINFO> ajob;   // (an operation: its parts in order)
    if (row.iop >= 0)
        for (int ij : s_aaop[m.iep][size_t(row.iop)].aijob) ajob.push_back(s_aajob[m.iep][size_t(ij)]);
    else ajob.push_back(s_aajob[m.iep][size_t(row.ijob)]);
    const JOBINFO j = ajob.front();
    s_rep.fActive = true;
    s_rep.iep = m.iep;
    s_rep.job = j;
    s_rep.ajobNext.assign(ajob.begin() + 1, ajob.end());
    s_rep.gsKeep = g_gamesave;
    if (FPrint()) std::printf("replay: job %d '%s' (episode %d, %zu more parts): task %d in world %d\n", j.oidJob, j.strName.c_str(), m.iep,
                              s_rep.ajobNext.size(), j.oidTaskFirst, j.iwrecFirst);
    CloseReplayMenu();
    // the job (an operation: each part) as on its first play: its and its tasks' played / done counts and closed
    // flags cleared (the save keeps the real ones: s_rep.gsKeep, merged back at the end), so the intro scenes that
    // only play the first time play
    for (const JOBINFO& jp : ajob)
    {
        for (const PROGREF& pr : jp.aprCount)
        {
            CRef r;
            r.SetS32(0);
            FSetProgressKey(pr.akey, r);
        }
        for (const PROGREF& pr : jp.aprFlag)
        {
            CRef r;
            r.SetBool(false);
            FSetProgressKey(pr.akey, r);
        }
    }
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

bool FReplayJobEnd(SW* psw, JOB* pjob, bool fComplete)
{
    if (!s_rep.fActive || !pjob || pjob->oid != s_rep.job.oidJob) return false;
    if (fComplete && !s_rep.ajobNext.empty())
    {
        // an operation's part complete: the game's own chain goes on to the next part (its first task requested)
        s_rep.job = s_rep.ajobNext.front();
        s_rep.ajobNext.erase(s_rep.ajobNext.begin());
        if (FPrint()) std::printf("replay: job %d complete (t %.2f): on to the operation's next part %d '%s'\n", pjob->oid, g_clock.t,
                                  s_rep.job.oidJob, s_rep.job.strName.c_str());
        return false;
    }
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
    if (td.kind == 0 && td.id == IdText(0x6D + iep))   // the episode's name (0x6D + episode, as the job help's 0x51B): the selected row's colours
    {
        if (!FShadow(td))
        {
            s_tdTitle = td;
            s_fTitle = true;
        }
        if (td.ptext && td.pfont && iep <= 8)
        {
            const std::string str = StrWrapped(*td.pfont, td.pfontIcons, *td.ptext, td.size / 51.2f, td.wBox);
            s_aclineTitle[iep] = std::max(s_aclineTitle[iep], 1 + int(std::count(str.begin(), str.end(), '\n')));
        }
        return false;
    }
    const bool fStart = td.kind == 0 && td.id == IdText(0x6C), fPercent = td.kind == 1 && td.id == 0x52F, fTime = td.kind == 1 && td.id == 0x530;
    if (!fStart && !fPercent && !fTime) return false;
    const MENU& m = s_menu;
    if (!m.fShown || m.iep != iep)
    {
        // "(TRIANGLE): Replay Jobs" one line under the time, in the "(X): Start Episode" line's style
        if (fTime && !ArowOpen(iep).empty())
            Text(td, "&2T&." + StrPort(PTX_GlyphSep) + StrPort(PTX_ReplayJobs), td.x, td.y + 35.0f, td.size, 0.0f, td.off, td.sc);
        // (PC menus) with the keyboard / mouse, this screen's back button after "(X): Start Episode", in its
        // style ("(E): Back", the bound key's keycap; the game's "Back" 0x40). A controller keeps the original line.
        if (fStart && td.ptext && td.pfont && PromptKeyboardActive())
        {
            const bool fReplay = !ArowOpen(iep).empty();
            const char* szBack = !fReplay ? SzMenuBackGlyph() : BtnReplayBack() == 0x20 ? "&2O&." : "&2X&.";
            const float dx = DxTextUi(*td.pfont, td.pfontIcons, *td.ptext, td.size / 51.2f) * td.sx;
            Text(td, std::string(szBack) + StrPort(PTX_GlyphSep) + StrLocalized(g_psw, IdText(0x40)),
                 td.x + dx + (FWideChars() ? td.size : td.size * 1.2f), td.y, td.size, 0.0f, td.off, td.sc);
        }
        return false;
    }
    if (!fStart) return true;
    // the rows (the selected one in the episode name's colours), the description, the buttons
    const std::vector<JOBINFO>& ajob = s_aajob[iep];
    const float size = td.size * 0.8f, dy = 28.0f;
    const int crowShown = CrowShown(m.iep);
    const float y0 = td.y - 50.0f + float(CROW_SHOWN - crowShown) * dy;
    const bool fText = !FShadow(td);
    const glm::vec4 offSel = s_fTitle && fText ? s_tdTitle.off : td.off, scSel = s_fTitle && fText ? s_tdTitle.sc : td.sc;
    const int cRow = std::min<int>(crowShown, int(m.arow.size()));
    // (an operation's part: "1. ", "2. "...: the first part of a game-named operation is its "Operation:" job, of the
    // same name)
    auto strName = [&](const ROW& row) {
        if (row.iop >= 0) return s_aaop[iep][size_t(row.iop)].strName;
        return (row.ipart >= 0 ? std::to_string(row.ipart + 1) + ". " : std::string()) + ajob[size_t(row.ijob)].strName;
    };
    for (int r = 0; r < cRow; ++r)
    {
        const int i = m.itop + r;
        if (i >= int(m.arow.size())) break;
        const ROW& row = m.arow[size_t(i)];
        const bool fSel = i == m.isel;
        Text(td, strName(row), td.x + 22.0f + (row.ipart >= 0 ? 28.0f : 0.0f), y0 + dy * float(r), size, 0.0f, fSel ? offSel : td.off,
             fSel ? scSel : td.sc);
    }
    const ROW& rowSel = m.arow[size_t(m.isel)];
    const std::string& strDesc = rowSel.iop >= 0 ? s_aaop[iep][size_t(rowSel.iop)].strDesc : ajob[size_t(rowSel.ijob)].strDesc;
    const float yDesc = td.y - 50.0f + dy * float(CROW_SHOWN) + 6.0f;
    Text(td, strDesc, td.x + 22.0f, yDesc, td.size * 0.55f, 500.0f, td.off, td.sc);
    // (the region's confirm / back buttons, the game's "Back" 0x40; the Japanese with its full-width space)
    const std::string strHint = SzMenuConfirmGlyph() + StrPort(PTX_GlyphSep) + StrPort(PTX_ReplayJob) +
                                (FWideChars() ? "\xE3\x80\x80" : "    ") + SzMenuBackGlyph() + StrPort(PTX_GlyphSep) +
                                StrLocalized(g_psw, IdText(0x40));
    Text(td, strHint, td.x, yDesc + 44.0f, td.size * 0.7f, 0.0f, td.off, td.sc);
    // (the icon font has no arrows: the position in the list after the buttons when it scrolls)
    if (int(m.arow.size()) > crowShown && td.pfont)
    {
        const std::string str = std::to_string(m.isel + 1) + " / " + std::to_string(m.arow.size());
        const float dx = DxTextUi(*td.pfont, td.pfontIcons, strHint, td.size * 0.7f / 51.2f) * td.sx;
        Text(td, str, td.x + dx + 40.0f, yDesc + 44.0f, td.size * 0.7f, 0.0f, td.off, td.sc);
    }
    return true;
}
}  // namespace
