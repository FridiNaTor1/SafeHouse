// See epmenu3.h. Ported from the Sly 3 asm (orig3 addresses in the comments).

#include "epmenu3.h"

#include "card3.h"
#include "cls.h"
#include "disc.h"
#include "frontend.h"
#include "lo.h"
#include "menu.h"
#include "region.h"
#include "savedata.h"
#include "screen3.h"
#include "screenrt.h"
#include "sound.h"
#include "sw.h"
#include "transition.h"
int NRandInRange(int nLow, int nHigh);   // (util.h: 0x11D3F8)
#include "worldaccess.h"
#include "game/sly3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
uint32_t UElf(uint32_t va) { return g_disc.U32Elf(va); }
int NElf(uint32_t va) { return int(g_disc.U32Elf(va)); }

// the executable's tables
constexpr uint32_t VA_ALL_OPEN = 0x3448E0;     // non-zero: every episode open (a development flag; 0 on the disc)
constexpr uint32_t VA_BG_LISTS = 0x3449E8;     // 8 x {count, parts*}: the backdrop's idle parts per episode group
constexpr uint32_t VA_BG_GROUP = 0x344A28;     // S32[9]: the last backdrop group open for an episode
constexpr uint32_t VA_PAGES = 0x344D98;        // 9 x {count, entries*}: an episode's page (0x18-byte entries)
constexpr uint32_t VA_INTRO_MOVIE = 0x344DE0;  // 9 movie keys: played before an episode's first start
constexpr uint32_t VA_PART_STATE = 0x344E08;   // S32[32]: the part a state plays
constexpr uint32_t VA_PART_PAGE_IN = 0x344E88; // S32[9]: state 6's (the page coming in) per episode
constexpr uint32_t VA_PART_PAGE = 0x344EB0;    // state 7's (the page)
constexpr uint32_t VA_PART_PAGE_OUT = 0x344ED8;// state 10's
constexpr uint32_t VA_NAMES = 0x344F00;        // S32[9]: the episodes' names (text ids)
constexpr uint32_t VA_JOBS = 0x349B20;         // 9 x {count, char**}: an episode's jobs (its completion)
constexpr uint32_t VA_CHALLENGES = 0x349B68;   // 9 x {count, char**}: its challenges (the percent)
// the parts (Sly 2's 0x3xx + 103)
constexpr int PART_LIST_SHOW = 0x382, PART_UP = 0x384, PART_DOWN = 0x383;
constexpr int PART_PREV_ON = 0x64F, PART_PREV_OFF = 0x650, PART_NEXT_ON = 0x651, PART_NEXT_OFF = 0x652;
constexpr int SND_REFUSED = 0x36;   // (PsndNewPlain(0x36): an end of the wheel / a closed entry)

// an entry of an episode's page (0x18 bytes)
struct EPENT
{
    int kind = 0;           // +0x00: 0 start the episode, 1 a movie, 2 the challenges' list, 3 the gallery, 4 the movies' list
    int idNormal = -1;      // +0x04 / +0x08 / +0x0C: its part when open, chosen, closed
    int idChosen = -1;
    int idClosed = -1;
    uint32_t vaMovie = 0;   // +0x10: a movie's key (kind 1)
    uint32_t vaPath = 0;    // +0x14: its task's name (kind 1: open once that task is complete; 0 always)
};
EPENT EntAt(uint32_t va)
{
    EPENT e;
    e.kind = NElf(va);
    e.idNormal = NElf(va + 4);
    e.idChosen = NElf(va + 8);
    e.idClosed = NElf(va + 0xC);
    e.vaMovie = UElf(va + 0x10);
    e.vaPath = UElf(va + 0x14);
    return e;
}

// a shuffled bag (0x2BDF40 / 0x2B4FF0: every item once in a random order, no item twice in a row across refills)
struct BAG
{
    std::vector<int> a;
    std::size_t i = 0;
    int Next()
    {
        if (a.empty()) return -1;
        if (i >= a.size())
        {
            const int last = a.back();
            for (std::size_t k = a.size() - 1; k > 0; --k) std::swap(a[k], a[std::size_t(NRandInRange(0, int(k)))]);
            if (a.size() > 2 && a[0] == last) std::swap(a[0], a[std::size_t(NRandInRange(1, int(a.size()) - 1))]);
            i = 0;
        }
        return a[i++];
    }
};

struct EPMENU3
{
    LO* plo = nullptr;
    int cur = 0;            // +0x124
    int max = 8;            // +0x128
    int isel = 0;           // +0x12C
    void* plist = nullptr;  // +0x130 (the lists of 0x148 + 12 cur: the challenges, the gallery, the movies; not loaded
                            // natively yet: none, so their entries are closed)
    float zoom = 1.0f, xPan = 0.0f, yPan = 0.0f;   // +0x134 / +0x138 / +0x13C (the gallery's view)
    int state = -1;         // +0x140
    int pending = -1;       // +0x144
    int idCur = -1;         // +0x1E8 the backdrop's idle part playing
    int idOld = -1;         // +0x1EC the one fading out
    std::vector<BAG> abag;  // +0x1B4: per backdrop group
    BAG bagGroups;          // +0x1D4: over the groups
};
EPMENU3 s_epm3;
SW* s_pswEpm = nullptr;
bool s_fActive = false;

// the game record's "where the chooser returns to" (GAME +0x1488 the state, +0x148C the episode, +0x1490 the entry,
// +0x1494 the list's item): set before a movie or a challenge reloads dvd_menu; -1 none (cleared by 0x185638)
struct EPRETURN { int state = -1, cur = 0, isel = 0, item = 0; };
EPRETURN s_ret;

const char* SzEp(int iep) { return sly3::SzEpisode(iep); }

int CEntries(int iep) { return iep >= 0 && iep < 9 ? NElf(VA_PAGES + 8 * uint32_t(iep)) : 0; }
EPENT Ent(int iep, int i) { return EntAt(UElf(VA_PAGES + 8 * uint32_t(iep) + 4) + 0x18 * uint32_t(i)); }

// orig3: 0x0017BFF0 / 0x0017C018: the current episode's page, its chosen entry
bool FCurEntry(const EPMENU3& p, EPENT* pe)
{
    const int c = CEntries(p.cur);
    if (c <= 0 || p.isel < 0 || p.isel >= c) return false;
    *pe = Ent(p.cur, p.isel);
    return true;
}

// orig3: 0x0017C1C0: the entry can be chosen
bool FEntryOpen(const EPMENU3& p, const EPENT& e)
{
    if (UElf(VA_ALL_OPEN)) return true;
    switch (e.kind)
    {
    case 0:   // the Intro and the last episode cannot be started again once complete
        if (p.cur == 1 || p.cur == 8) return !FEpisodeComplete3(p.cur);
        return true;
    case 1:
    {
        if (!e.vaPath) return true;
        // the movie is open when its task is complete: "<task>" "is_complete" (0x21CDA8 with 0x3215E8)
        const CRef r = RefGetProgress({g_disc.StrElf(e.vaPath), "is_complete"});
        return r.m_tagk == TAGK_Bool ? r.m_bool : r.m_tagk == TAGK_S32 ? r.m_n != 0 : r.m_tagk == TAGK_F32 ? r.m_g != 0.0f : false;
    }
    case 2: case 3: case 4:
        return p.plist != nullptr;   // (the list's +0x5C: TODO(sly3): the lists are not loaded)
    default: return true;
    }
}

// orig3: 0x0017C078 / 0x0017C118: the next / previous open entry (wrapping)
void NextEntry(EPMENU3& p, int d)
{
    const int c = CEntries(p.cur);
    if (c <= 0) return;
    const int start = p.isel;
    do
    {
        p.isel = ((p.isel + d) % c + c) % c;
        if (FEntryOpen(p, Ent(p.cur, p.isel))) return;
    } while (p.isel != start);
}

// orig3: 0x0017BF68: the part of the state
int IdPartState(const EPMENU3& p)
{
    const uint32_t ic = uint32_t(std::max(0, std::min(p.cur, 8)));
    switch (p.state)
    {
    case 6: return NElf(VA_PART_PAGE_IN + 4 * ic);
    case 7: return NElf(VA_PART_PAGE + 4 * ic);
    case 10: return NElf(VA_PART_PAGE_OUT + 4 * ic);
    default: return p.state >= 0 && p.state < 32 ? NElf(VA_PART_STATE + 4 * uint32_t(p.state)) : -1;
    }
}

void Refused() { PsndStartSound(SND_REFUSED); }

// orig3: 0x0017C328
void SetEpState(EPMENU3& p, int s, int pending)
{
    if (p.state == s) return;
    switch (p.state)
    {
    case 0: HudAutoSaveFrontEnd(); break;   // (0x1CFE08 on the card 0x375290)
    case 3: --p.cur; break;
    case 4: ++p.cur; break;
    case 5:
        if (s == 6)
        {
            p.isel = CEntries(p.cur) - 1;
            NextEntry(p, 1);   // (the first open one)
        }
        break;
    case 15: case 20: case 31: p.plist = nullptr; break;
    case 25: if (s != 26) p.plist = nullptr; break;
    case 26: p.zoom = 1.0f; break;   // (0x262990(0x64E, the identity))
    default: break;
    }
    p.pending = pending;
    p.state = s;
    switch (s)
    {
    case 1: case 2: SelectScreenObjPart(p.plo, PART_LIST_SHOW); break;
    case 3: SelectScreenObjPart(p.plo, PART_UP); break;
    case 4: SelectScreenObjPart(p.plo, PART_DOWN); break;
    case 6: case 7: case 10:
    {
        // the page's entries: open (chosen or not) or closed
        const int c = CEntries(p.cur);
        for (int i = 0; i < c; ++i)
        {
            const EPENT e = Ent(p.cur, i);
            SelectScreenObjPart(p.plo, FEntryOpen(p, e) ? (p.isel == i ? e.idChosen : e.idNormal) : e.idClosed);
        }
        break;
    }
    case 8: NextEntry(p, -1); break;
    case 9: NextEntry(p, 1); break;
    case 11: case 16: case 21: case 27:
    case 13: case 18: case 23: case 29:
    case 14: case 19: case 24: case 30:
        // the lists (two player 0x148, challenges 0x148, gallery 0x14C, movies 0x150 of the episode): their arrows
        // (TODO(sly3): the list objects; none natively: no previous / next)
        SelectScreenObjPart(p.plo, PART_PREV_OFF);
        SelectScreenObjPart(p.plo, PART_NEXT_OFF);
        break;
    default: break;
    }
    p.xPan = p.yPan = 0.0f;
    p.zoom = 1.0f;
    const int id = IdPartState(p);
    if (id != -1 && id != IdScreenObjPartCur(p.plo)) ShowScreenObj(p.plo, id);   // (vt slot 53)
}

// orig3: 0x0017C698: the state after the current one
void ToPending(EPMENU3& p)
{
    const int s = p.pending;
    p.pending = -1;
    SetEpState(p, s, -1);
}

// orig3: 0x0017BDE8: the backdrop's idle parts (one plays; with half a second left the next starts over it)
void UpdateBackdrop(EPMENU3& p)
{
    if (p.idOld != -1 && !FScreenObjBusy(p.plo, p.idOld))
    {
        ResetScreenObjPart(p.plo, p.idOld);
        p.idOld = -1;
    }
    if (p.idCur != -1 && !FScreenObjBusy(p.plo, p.idCur))
    {
        ResetScreenObjPart(p.plo, p.idCur);
        p.idCur = -1;
    }
    if (p.state >= 2 && p.state < 5)
    {
        if (p.idOld == -1 && p.idCur != -1 && DtScreenObjPartLeft(p.plo, p.idCur) <= 0.5f)
        {
            p.idOld = p.idCur;
            p.idCur = -1;
        }
        if (p.idCur == -1)
        {
            const int g = p.bagGroups.Next();
            const int id = g >= 0 && std::size_t(g) < p.abag.size() ? p.abag[std::size_t(g)].Next() : -1;
            if (id != -1)
            {
                SelectScreenObjPart(p.plo, id);
                p.idCur = FScreenObjBusy(p.plo, id) ? id : -1;
            }
        }
        return;
    }
    for (int* pid : {&p.idOld, &p.idCur})
        if (*pid != -1)
        {
            ResetScreenObjPart(p.plo, *pid);
            *pid = -1;
        }
}

uint32_t TakeBtn(uint32_t grf) { return GrfbtnTakePressed(grf); }
}  // namespace

// ------------------------------------------------------------ the episodes' progress
bool FEpisodeComplete3(int iep)
{
    if (iep < 0 || iep >= 9) return false;
    const int c = NElf(VA_JOBS + 8 * uint32_t(iep));
    if (c <= 0) return false;
    const uint32_t va = UElf(VA_JOBS + 8 * uint32_t(iep) + 4);
    for (int i = 0; i < c; ++i)
    {
        const CRef r = RefGetProgress({g_disc.StrElf(UElf(va + 4 * uint32_t(i))), "is_complete"});
        if (!(r.m_tagk == TAGK_Bool && r.m_bool)) return false;
    }
    return true;
}

float DtEpisode3(int iep)
{
    if (iep < 0 || iep >= 9) return 0.0f;
    const CRef r = RefGetProgress({"episode", SzEp(iep), "dt"});
    return r.m_tagk == TAGK_F32 ? r.m_g : r.m_tagk == TAGK_S32 ? float(r.m_n) : 0.0f;
}

int NEpisodePercent3(int iep)
{
    if (iep < 0 || iep >= 9) return 0;
    int c = 0, cDone = 0;
    for (uint32_t vaTab : {VA_JOBS, VA_CHALLENGES})
    {
        const int n = NElf(vaTab + 8 * uint32_t(iep));
        const uint32_t va = UElf(vaTab + 8 * uint32_t(iep) + 4);
        for (int i = 0; i < n; ++i)
        {
            ++c;
            const CRef r = RefGetProgress({g_disc.StrElf(UElf(va + 4 * uint32_t(i))), "is_complete"});
            if (r.m_tagk == TAGK_Bool && r.m_bool) ++cDone;
        }
    }
    if (c == 0 || cDone == 0) return 0;
    if (cDone == c) return 100;
    return std::max(1, std::min(99, cDone * 100 / c));
}

void StartEpisode3(SW* psw, int iep)
{
    if (iep < 0 || iep >= 9) return;
    std::printf("flow: episode %d %s started from the chooser\n", iep, SzEp(iep));
    // orig3 0x289198: the Intro not complete: its binocucom task 1798 in m_ext (35); else the episode's first world
    // (0x2EDFB0) at the default point. (TODO(sly3): its progress "episodeCurrent" and the saved checkpoint task /
    // world it resumes at (SW +0x4074..+0x4080), the China / Pirate special cases.)
    if (iep == sly3::EP_Intro && !FEpisodeComplete3(iep))
    {
        RequestTaskStart(psw, 35, 1798);
        return;
    }
    const int iworld = NElf(sly3::VA_EPISODE_WORLDS + 4u * uint32_t(iep));
    if (iworld < 0) return;
    RequestWorldWarp(psw, iworld, sly3::POINT_DEFAULT, -1);
}

// ------------------------------------------------------------ the chooser
void SetEpMenuReturn3(int iep)
{
    if (s_ret.state != -1) return;
    s_ret = EPRETURN{iep == sly3::EP_Trainer ? 1 : 6, iep, 0, 0};
}

void StartEpMenu3(SW* psw)
{
    s_fActive = false;
    s_epm3 = EPMENU3{};
    s_pswEpm = psw;
    LO* plo = nullptr;
    for (LO* p : AploOfClass(psw, S3CID(0x83)))
    {
        plo = p;
        break;
    }
    if (!plo) return;
    EPMENU3& p = s_epm3;
    p.plo = plo;
    // (debug) SLY3_EPMDONE=job,job,..: those jobs' "is_complete" set (e.g. m0_jbintro,m0_navigate,m0_climb: Venice open)
    if (const char* sz = std::getenv("SLY3_EPMDONE"))
    {
        std::string str = sz;
        for (std::size_t i = 0; i <= str.size();)
        {
            const std::size_t j = std::min(str.find(',', i), str.size());
            CRef r;
            r.SetBool(true);
            if (j > i) FSetProgress({str.substr(i, j - i), "is_complete"}, r);
            i = j + 1;
        }
    }
    s_fActive = true;
    ClearJoySteps();   // (0x1A99D0 on +0x1F0 with the pad)
    // the backdrop's groups: up to the one of the last complete episode
    int iepDone = 8;
    while (iepDone >= 2 && !UElf(VA_ALL_OPEN) && !FEpisodeComplete3(iepDone)) --iepDone;
    const int cgroup = NElf(VA_BG_GROUP + 4 * uint32_t(std::max(0, iepDone)));
    for (int i = 0; i <= cgroup && i < 8; ++i)
    {
        BAG bag;
        const int c = NElf(VA_BG_LISTS + 8 * uint32_t(i));
        const uint32_t va = UElf(VA_BG_LISTS + 8 * uint32_t(i) + 4);
        for (int k = 0; k < c; ++k) bag.a.push_back(NElf(va + 4 * uint32_t(k)));
        bag.i = bag.a.size();
        p.abag.push_back(std::move(bag));
        p.bagGroups.a.push_back(i);
    }
    p.bagGroups.i = p.bagGroups.a.size();
    // the last episode open: one played, or whose previous one is complete
    p.max = 8;
    while (p.max >= 2 && !UElf(VA_ALL_OPEN) && DtEpisode3(p.max) == 0.0f && !FEpisodeComplete3(p.max - 1)) --p.max;
    switch (s_ret.state)
    {
    case 1: p.cur = s_ret.cur; SetEpState(p, 1, -1); break;
    case 6: p.cur = s_ret.cur; p.isel = s_ret.isel; SetEpState(p, 6, -1); break;
    case 11: case 16: case 21: case 26: p.cur = s_ret.cur; SetEpState(p, s_ret.state, -1); break;
    default: p.cur = p.max; SetEpState(p, 0, -1); break;
    }
    s_ret = EPRETURN{};   // (0x185638)
    if (std::getenv("SLY3_EPMPRINT"))
        std::printf("epmenu3: oid %d, episode %d of %d (Intro complete %d, Trainer %d, Venice time %.0f)\n", plo->oid,
                    p.cur, p.max, FEpisodeComplete3(1), FEpisodeComplete3(2), DtEpisode3(3));
}

bool FEpMenu3Active() { return s_fActive && s_epm3.plo; }

void UpdateEpMenu3(SW* psw)
{
    if (!FEpMenu3Active()) return;
    EPMENU3& p = s_epm3;
    // (the menu 0x46F550, the card 0x3751D0, the title 0x46FBE0 take the input first)
    if (FMenuShown() || FCardMessageShown3() || FTitleShown())
    {
        if (std::getenv("SLY3_EPMPRINT") && std::getenv("SLY3_EPMPRINT")[0] == '2')
            std::printf("epmenu3: waits (menu %d card %d title %d)\n", FMenuShown(), FCardMessageShown3(), FTitleShown());
        return;
    }
    if (std::getenv("SLY3_EPMPRINT") && std::getenv("SLY3_EPMPRINT")[0] == '2')
        std::printf("epmenu3: state %d part %d busy %d cur part %d\n", p.state, IdPartState(p), FScreenObjBusy(p.plo, IdPartState(p)), IdScreenObjPartCur(p.plo));
    UpdateBackdrop(p);
    const int idPart = IdPartState(p);
    auto busy = [&](int id) { return FScreenObjBusy(p.plo, id); };
    const int sOld = p.state;
    switch (p.state)
    {
    case 0: if (!busy(idPart)) SetEpState(p, 1, -1); break;
    case 1: if (!busy(idPart)) SetEpState(p, 2, -1); break;
    case 3: if (!busy(PART_UP)) SetEpState(p, 2, -1); break;
    case 4: if (!busy(PART_DOWN)) SetEpState(p, 2, -1); break;
    case 2:   // the wheel
    {
        if (TakeBtn(BtnMenuConfirm()))
        {
            if (p.cur == 0) SetEpState(p, 5, 11);   // (Two Player: its list page)
            else if (p.cur == sly3::EP_Trainer) StartEpisode3(psw, p.cur);   // (the Hazard Room has no page)
            else SetEpState(p, 5, 6);
            break;
        }
        const int n = NStepUpDown();
        if (n == 0) break;
        if ((n < 0 && p.cur <= 0) || (n > 0 && p.cur >= p.max)) { Refused(); break; }
        SetEpState(p, n < 0 ? 3 : 4, -1);
        break;
    }
    case 5: case 10: case 15: case 20: case 25: case 31:
        if (!busy(idPart)) ToPending(p);
        break;
    case 6: case 8: case 9:
        if (!busy(idPart)) SetEpState(p, 7, -1);
        break;
    case 7:   // the episode's page
    {
        if (TakeBtn(BtnMenuConfirm()))
        {
            EPENT e;
            if (!FCurEntry(p, &e)) break;
            switch (e.kind)
            {
            case 0:   // Start / Continue Episode
                if (DtEpisode3(p.cur) == 0.0f && UElf(VA_INTRO_MOVIE + 4 * uint32_t(p.cur)))
                    QueueMovie(g_disc.PbElf(UElf(VA_INTRO_MOVIE + 4 * uint32_t(p.cur)), 64));
                StartEpisode3(psw, p.cur);
                break;
            case 1:   // a movie, then the chooser again on this entry
                s_ret = EPRETURN{6, p.cur, p.isel, 0};
                QueueMovie(g_disc.PbElf(e.vaMovie, 64));
                RequestWorldWarp(psw, 0, sly3::POINT_DEFAULT, -1);
                break;
            case 2: SetEpState(p, 10, 16); break;   // Jobs && Challenges
            case 3: SetEpState(p, 10, 21); break;   // Gallery
            case 4: SetEpState(p, 10, 27); break;   // Bonus Movies
            default: break;
            }
            break;
        }
        if (TakeBtn(BtnMenuBack())) { SetEpState(p, 10, 1); break; }
        const int n = NStepUpDown();
        if (n != 0) SetEpState(p, n < 0 ? 8 : 9, -1);
        break;
    }
    // the lists (Two Player 11..15, Jobs && Challenges 16..20, Gallery 21..26, Bonus Movies 27..31): no list object
    // natively yet (TODO(sly3): 0x148 + 12 cur), so nothing to choose; back goes back
    case 11: case 13: case 14: if (!busy(idPart)) SetEpState(p, 12, -1); break;
    case 16: case 18: case 19: if (!busy(idPart)) SetEpState(p, 17, -1); break;
    case 21: case 23: case 24: if (!busy(idPart)) SetEpState(p, 22, -1); break;
    case 27: case 29: case 30: if (!busy(idPart)) SetEpState(p, 28, -1); break;
    case 12: if (TakeBtn(BtnMenuBack())) SetEpState(p, 15, 1); break;
    case 17: if (TakeBtn(BtnMenuBack())) SetEpState(p, 20, 6); break;
    case 22: if (TakeBtn(BtnMenuBack())) SetEpState(p, 25, 6); break;
    case 26: if (TakeBtn(BtnMenuBack())) SetEpState(p, 21, -1); break;
    case 28: if (TakeBtn(BtnMenuBack())) SetEpState(p, 31, 6); break;
    default: break;
    }
    if (std::getenv("SLY3_EPMPRINT") && p.state != sOld)
        std::printf("epmenu3: state %d -> %d (episode %d, entry %d)\n", sOld, p.state, p.cur, p.isel);
}

bool FStrEpMenu3(int id, std::string* pstr)
{
    const EPMENU3& p = s_epm3;
    const SW* psw = s_pswEpm;
    auto text = [&](int idText) { return StrText3(psw, idText); };
    if (id >= 1468 && id <= 1477)
    {
        // orig3 0x17C730: the wheel's names around the episode shown (k - 3 from it)
        const int i = p.cur - 3 + (id - 1468);
        *pstr = FEpMenu3Active() && i >= 0 && i <= p.max ? text(NElf(VA_NAMES + 4 * uint32_t(i))) : std::string();
        return true;
    }
    switch (id)
    {
    case 1478: *pstr = FEpMenu3Active() ? text(DtEpisode3(p.cur) <= 0.0f ? 0x85 : 0x86) : ""; return true;   // 0x17C7A8
    case 1479:   // 0x17C848: "%d%%"
    {
        if (!FEpMenu3Active()) { pstr->clear(); return true; }
        char ach[16];
        std::snprintf(ach, sizeof(ach), "%d%%", NEpisodePercent3(p.cur));
        *pstr = ach;
        return true;
    }
    case 1480:   // 0x17C8A8: "%01d:%02d" the hours and minutes played
    {
        if (!FEpMenu3Active()) { pstr->clear(); return true; }
        const float dt = DtEpisode3(p.cur);
        char ach[24];
        std::snprintf(ach, sizeof(ach), "%01d:%02d", int(dt * (1.0f / 3600.0f)), int(std::fmod(dt, 3600.0f) * (1.0f / 60.0f)));
        *pstr = ach;
        return true;
    }
    case 1499: *pstr = text(0x4F) + " : " + SzMenuBackGlyph(); return true;   // 0x17C950 "Back : &2T&."
    case 1500:   // 0x17CFC0: the episode's name
        *pstr = FEpMenu3Active() && p.cur >= 0 && p.cur < 9 ? text(NElf(VA_NAMES + 4 * uint32_t(p.cur))) : "";
        return true;
    default:
        if (id >= 1481 && id <= 1498) { pstr->clear(); return true; }   // (the lists' strings: TODO(sly3))
        return false;
    }
}
