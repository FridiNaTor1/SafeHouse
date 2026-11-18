// See epmenu3.h. Ported from the Sly 3 asm (orig3 addresses in the comments).

#include "epmenu3.h"
#include "duel3.h"
#include "galleon3.h"
#include "clock.h"
#include "joy.h"
#include <cstring>
#include "hud.h"
#include "copsmap3.h"
#include "hackmp3.h"
#include "gauge3.h"
#include "cheat.h"

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
#include "game/loaders3.h"
#include "animfind.h"
#include "outfit3.h"
#include "challenge3.h"

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
constexpr uint32_t VA_MOVIE_KEYS0 = 0x3179F8;  // the movies' 64-byte keys (the Bonus Movies' first five rules)
// the parts (Sly 2's 0x3xx + 103)
constexpr int PART_LIST_SHOW = 0x382, PART_UP = 0x384, PART_DOWN = 0x383;
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

// the development flag 0x3448E0 (every episode and list row open; debug SLY3_EPMALLOPEN=1 as the reference's poke of it)
bool FAllOpen()
{
    static const bool s_fEnv = std::getenv("SLY3_EPMALLOPEN") != nullptr;
    return s_fEnv || UElf(VA_ALL_OPEN);
}

// ------------------------------------------------------------ the lists (class 0x9D, game/loaders3.h LOLIST9D3)
// orig3: SW +0x148: 9 episodes x 3 slots (0 the Two Player / Jobs && Challenges list (mode 0), 1 the Gallery (mode
// 1), 2 the Bonus Movies (mode 2)), registered by each list's post-load 0x17AB08; +0x130 the list shown
std::array<std::array<LOLIST9D3*, 3>, 9> s_aplist{};
LOLIST9D3* s_plistCur = nullptr;

int NProgressInt(const std::vector<int16_t>& akey)
{
    const CRef r = RefGetProgressKey(akey);
    return r.m_tagk == TAGK_Bool ? (r.m_bool ? 1 : 0) : r.m_tagk == TAGK_S32 ? r.m_n : r.m_tagk == TAGK_F32 ? int(r.m_g) : 0;
}
// a class-0xAA game object's key option (kind 22: 1454 +0x70 the key that opens it, 1456 +0xA8, 1457 +0xB0 its
// "done" key); empty: none
std::vector<int16_t> AkeyNode(const LO* pnode, int eopid)
{
    for (const OPT& o : pnode->aopt)
        if (o.eopid == eopid && !o.aval.empty()) return o.aval[0].an;
    return {};
}
// orig3: 0x002835D0: the game's "done" key (+0xB0) read (0 none)
int NNodeDone(const LO* pnode) { const auto akey = AkeyNode(pnode, 1457); return akey.empty() ? 0 : NProgressInt(akey); }
// orig3: 0x002833C8: a game of a list open: always with the development flag or once done, else its key (+0x70; none:
// open). (The key's context: the challenge's saved progress while one runs (0x2852B0: +0x38); the three card slots for
// a mode-2 game with the card state 6: TODO(sly3), the chooser runs with no challenge.)
bool FNodeOpen(const LO* pnode)
{
    if (FAllOpen() || NNodeDone(pnode)) return true;
    const auto akey = AkeyNode(pnode, 1454);
    if (akey.empty()) return true;
    return NProgressInt(akey) != 0;
}

// orig3: 0x0017AB08 (the list's post-load, slot 17): registered in its episode's slot, each row's object found and
// its open state, the first open row chosen
void RegisterList(SW* psw, LOLIST9D3* pl)
{
    if (pl->mode >= 0 && pl->mode < 3 && pl->iep >= 0 && pl->iep < 9) s_aplist[size_t(pl->iep)][size_t(pl->mode)] = pl;
    pl->fEnabled = false;
    for (LOLIST9D3::ROW& r : pl->arow)
    {
        r.fAvail = false;
        switch (pl->mode)
        {
        case 0:
            r.pnode = PloFindSwObjectByOid(psw, r.n);   // (0x17E1B8 kind 0x105: global)
            if (r.pnode) r.fAvail = FNodeOpen(r.pnode);
            break;
        case 1: r.fAvail = FEpisodeComplete3(pl->iep); break;   // (0x25E4A0: the episode's gallery)
        case 2:
        {
            // the movie's key against the executable's first five (0x3179F8 + 0x40 k, compared by +8 ^ +0x38; a key with
            // +4 ^ +0x3C zero is none): always, 35 / 70 % of the challenges (0x25E510), the game at 100 % (0x25E6D0),
            // the last episode complete
            auto w = [](const uint8_t* pb, int ib) { uint32_t u; std::memcpy(&u, pb + ib, 4); return u; };
            const uint8_t* pk = r.abKey.data();
            if ((w(pk, 4) ^ w(pk, 0x3C)) == 0) break;
            const uint32_t idKey = w(pk, 8) ^ w(pk, 0x38);
            for (int k = 0; k < 5; ++k)
            {
                const uint8_t* pe = g_disc.PbElf(VA_MOVIE_KEYS0 + 0x40u * uint32_t(k), 0x40);
                if (!pe || (w(pe, 8) ^ w(pe, 0x38)) != idKey) continue;
                switch (k)
                {
                case 0: r.fAvail = true; break;
                case 1: r.fAvail = NChallengePercent3() >= 35; break;
                case 2: r.fAvail = NChallengePercent3() >= 70; break;
                case 3: r.fAvail = NGamePercent3(g_gamesave) >= 100; break;
                default: r.fAvail = FEpisodeComplete3(8); break;
                }
                break;
            }
            break;
        }
        default: break;
        }
        if (FAllOpen()) r.fAvail = true;
        if (r.fAvail) pl->fEnabled = true;
        if (std::getenv("SLY3_EPMLISTPRINT"))
            std::printf("epmenu3: list %d (episode %d mode %d) row %d -> %d (cid 0x%X) open %d job %d 3-D %d\n", pl->oid, pl->iep, pl->mode,
                        int(&r - pl->arow.data()), r.pnode ? r.pnode->oid : -1, r.pnode ? r.pnode->Cid() : 0, int(r.fAvail),
                        r.pnode ? int(r.pnode->U32Field3(0, 0x48, 0xFFFFFFFFu)) : -1, r.pnode ? int(r.pnode->U32Field3(0, 0x54, 0)) : 0);
    }
    pl->isel = -1;
    for (int i = 0; i < int(pl->arow.size()); ++i)   // (0x17AF50: the next open row)
        if (pl->arow[size_t(i)].fAvail) { pl->isel = i; break; }
}
// orig3: 0x0017AE78 / 0x0017AEB8: an open row before / after the chosen one
bool FListPrev(const LOLIST9D3* pl)
{
    for (int i = pl->isel - 1; i >= 0; --i) if (pl->arow[size_t(i)].fAvail) return true;
    return false;
}
bool FListNext(const LOLIST9D3* pl)
{
    for (int i = pl->isel + 1; i < int(pl->arow.size()); ++i) if (pl->arow[size_t(i)].fAvail) return true;
    return false;
}
// orig3: 0x0017AF08 / 0x0017AF50
void ListStep(LOLIST9D3* pl, int d)
{
    for (int i = pl->isel + d; i >= 0 && i < int(pl->arow.size()); i += d)
        if (pl->arow[size_t(i)].fAvail) { pl->isel = i; return; }
}
// orig3: 0x0017C6C8: the chosen row's object of the list shown (a mode-0 list of the episode shown)
LO* PnodeCur(int iepCur)
{
    const LOLIST9D3* pl = s_plistCur;
    if (!pl || pl->mode != 0 || pl->iep != iepCur || pl->isel < 0 || pl->isel >= int(pl->arow.size())) return nullptr;
    return pl->arow[size_t(pl->isel)].pnode;
}
// the parts of the list's arrows (1615 / 1616 previous on / off, 1617 / 1618 next)
constexpr int PART_LPREV_ON = 1615, PART_LPREV_OFF = 1616, PART_LNEXT_ON = 1617, PART_LNEXT_OFF = 1618;

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
    if (FAllOpen() || FCheatAllEpisodes()) return true;   // (0x3448E0: the cheat code R1 U U R1 R1 D, cheat.h)
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
    {
        // the episode's list of slot kind - 2 (Jobs && Challenges, the Gallery, the Bonus Movies) with an open row (+0x5C)
        const LOLIST9D3* pl = p.cur >= 0 && p.cur < 9 ? s_aplist[size_t(p.cur)][size_t(e.kind - 2)] : nullptr;
        return pl && pl->fEnabled;
    }
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
    case 15: case 20: case 31: p.plist = nullptr; s_plistCur = nullptr; break;
    case 25: if (s != 26) { p.plist = nullptr; s_plistCur = nullptr; } break;
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
    {
        // the lists (Two Player / Jobs && Challenges: slot 0, Gallery 1, Bonus Movies 2 of the episode): the list
        // shown taken (its text group streamed: 0x16F2B0(+0x54, its oid)), the row stepped, the arrows
        if (s == 11 || s == 16 || s == 21 || s == 27)
        {
            const size_t slot = s == 27 ? 2 : s == 21 ? 1 : 0;
            s_plistCur = p.cur >= 0 && p.cur < 9 ? s_aplist[size_t(p.cur)][slot] : nullptr;
            p.plist = s_plistCur;
            if (s_plistCur)
                if (LO* powner = PloFindSwObjectByOid(s_pswEpm, OID(s_plistCur->U32Field3(0, 0x54, 0xFFFFFFFFu))))
                    FStreamGroup97(s_plistCur->oid, powner);
        }
        else if (s_plistCur) ListStep(s_plistCur, s == 13 || s == 18 || s == 23 || s == 29 ? -1 : 1);
        const bool fPrev = s_plistCur && FListPrev(s_plistCur), fNext = s_plistCur && FListNext(s_plistCur);
        SelectScreenObjPart(p.plo, fPrev ? PART_LPREV_ON : PART_LPREV_OFF);
        SelectScreenObjPart(p.plo, fNext ? PART_LNEXT_ON : PART_LNEXT_OFF);
        break;
    }
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

int NGamePercent3(const GAMESAVE& gs)
{
    constexpr uint32_t VA_WEIGHTS = 0x349BB0;   // S32[9]
    const GAMESAVE saved = g_gamesave;
    g_gamesave = gs;
    int total = 0, count = 0;
    for (int i = 0; i < 9; ++i)
    {
        const int w = NElf(VA_WEIGHTS + 4 * uint32_t(i));
        count += NEpisodePercent3(i) * w;
        total += w * 100;
    }
    g_gamesave = saved;
    if (total == 0 || count == 0) return 0;
    if (count == total) return 100;
    return std::max(1, std::min(99, count * 100 / total));
}

int NChallengePercent3()
{
    // (0x25E348 per episode: the challenges of 0x349B68 and how many are complete)
    int total = 0, count = 0;
    for (int iep = 0; iep < 9; ++iep)
    {
        const int n = NElf(VA_CHALLENGES + 8 * uint32_t(iep));
        const uint32_t va = UElf(VA_CHALLENGES + 8 * uint32_t(iep) + 4);
        for (int i = 0; i < n; ++i)
        {
            ++total;
            const CRef r = RefGetProgress({g_disc.StrElf(UElf(va + 4 * uint32_t(i))), "is_complete"});
            if (r.m_tagk == TAGK_Bool && r.m_bool) ++count;
        }
    }
    if (total == 0 || count == 0) return 0;
    if (count == total) return 100;
    return std::max(1, std::min(99, count * 100 / total));
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
    int iworld = NElf(sly3::VA_EPISODE_WORLDS + 4u * uint32_t(iep));
    // (audit 4 CORE-18) 0x289198's special cases: China (6) starts in world 0x18 until "t4_recon_complete" is; the
    // Pirate episode (7) in world 0x20 once "t5_treasure_complete" is and while "t5_heist_complete" is not; the Isle (8)
    // sets episodeCurrent / episodeBest to 8 and resumes at the checkpoint-force task "oidCheckpointForce" in world
    // "widCheckpointForce" (written by the jobs' activations / rewards, game/task3.cpp), task 0x731 when it is 0 / -1.
    // SLY3_EPSTART2=1: the episode's first world always, as before.
    static const bool s_fOld = std::getenv("SLY3_EPSTART2") != nullptr;
    auto nProg = [](std::initializer_list<const char*> a) {
        std::vector<std::string> as(a.begin(), a.end());
        const CRef r = RefGetProgress(as);
        return r.m_tagk == TAGK_Bool ? int(r.m_bool) : r.m_tagk == TAGK_S32 ? r.m_n : 0;
    };
    if (!s_fOld)
    {
        if (iep == sly3::EP_China && !nProg({"t4_recon_complete", "is_complete"})) iworld = 0x18;
        else if (iep == sly3::EP_Pirate && nProg({"t5_treasure_complete", "is_complete"}) && !nProg({"t5_heist_complete", "is_complete"}))
            iworld = 0x20;
        else if (iep == sly3::EP_Isle)
        {
            CRef r;
            r.SetS32(iep);
            FSetProgress({"episodeCurrent"}, r);
            FSetProgress({"episodeBest"}, r);
            int oid = nProg({"oidCheckpointForce"});
            if (oid == -1 || oid == 0) oid = 0x731;
            else iworld = nProg({"widCheckpointForce"});
            if (iworld < 0) return;
            std::printf("flow: the Isle resumes at task %d in world %d\n", oid, iworld);
            RequestTaskStart(psw, iworld, oid);
            return;
        }
    }
    if (iworld < 0) return;
    RequestWorldWarp(psw, iworld, sly3::POINT_DEFAULT, -1);
}

// ------------------------------------------------------------ the chooser
void SetEpMenuReturn3(int iep)
{
    if (s_ret.state != -1) return;
    s_ret = EPRETURN{iep == sly3::EP_Trainer ? 1 : 6, iep, 0, 0};
}

// (hud.h) the list pages' picture slots (dvd_menu's kind-7 elements 0x649..0x64E): the chosen row's picture, the kind-2
// item of the list's streamed group (0x16F2B0 of the text owner +0x54: one shader per row, in the rows' order)
// (hud.h) the Gallery's picture (state 26: the element 0x64E zoomed by +0x134 about the panned point +0x138 / +0x13C:
// 0x262990 with D_00317FC0 scaled, the point at the canvas centre)
bool FQuadXf3(int idTexture, float a[6])
{
    if (!FEpMenu3Active() || idTexture != 0x64E || s_epm3.state != 26) return false;
    const EPMENU3& p = s_epm3;
    const float px = p.xPan * 320.0f + 320.0f, py = p.yPan * -240.0f + 240.0f;
    a[0] = p.zoom; a[1] = 0.0f; a[2] = 0.0f; a[3] = p.zoom;
    a[4] = -px * p.zoom + 320.0f; a[5] = -py * p.zoom + 240.0f;
    return true;
}

int OidEpisodeThumbnail3(int iep)
{
    static std::array<int, 9> s_aoid{};
    static uint32_t s_serial = 0;
    if (!s_pswEpm || iep < 0 || iep >= 9) return -1;
    if (s_serial != s_pswEpm->serial) { s_aoid.fill(-2); s_serial = s_pswEpm->serial; }
    if (s_aoid[size_t(iep)] != -2) return s_aoid[size_t(iep)];
    s_aoid[size_t(iep)] = -1;
    const LOLIST9D3* pl = s_aplist[size_t(iep)][0];
    LO* powner = pl ? PloFindSwObjectByOid(s_pswEpm, OID(pl->U32Field3(0, 0x54, 0xFFFFFFFFu))) : nullptr;
    if (!powner || !FStreamGroup97(pl->oid, powner)) return -1;
    s_aoid[size_t(iep)] = OidStreamedShader97(pl->oid, 0);
    // (the list shown streamed again: its own pictures; the episode's picture's shader stays loaded)
    if (s_plistCur)
        if (LO* pownerCur = PloFindSwObjectByOid(s_pswEpm, OID(s_plistCur->U32Field3(0, 0x54, 0xFFFFFFFFu))))
            FStreamGroup97(s_plistCur->oid, pownerCur);
    return s_aoid[size_t(iep)];
}

int IdQuadShaderAlias3(int idTexture)
{
    // (PC, copsmap3.h: the map select's hub on the Two Player page: Cops and Robbers' picture slot shows the hub's
    // episode's first replay picture; Venice (the original game) keeps the original picture)
    if (idTexture == 0x64B && cops3::FCopsNode(PnodeTwoPlayerShown3()) && cops3::IHubChosen() > 0)
        if (const int oid = OidEpisodeThumbnail3(cops3::IepHub(cops3::IHubChosen())); oid != -1) return oid;
    if (!FEpMenu3Active() || !s_plistCur || idTexture < 0x649 || idTexture > 0x64E) return -1;
    const LOLIST9D3* pl = s_plistCur;
    if (pl->isel < 0) return -1;
    // (the slots' binding is not in the decomp's C; inferred from what the pages draw: the chosen row in 0x64B (and the
    // Gallery's full view 0x64E), the open rows before it in 0x64A / 0x649, after it in 0x64C / 0x64D: the vertical
    // lists slide 0x64A / 0x64C in, the film strips show 0x64A | 0x64B | 0x64C)
    static const int s_ad[6] = {-2, -1, 0, 1, 2, 0};
    const int d = s_ad[idTexture - 0x649];
    int i = pl->isel;
    for (int k = 0; k != d;)
    {
        const int step = d < 0 ? -1 : 1;
        do i += step;
        while (i >= 0 && i < int(pl->arow.size()) && !pl->arow[size_t(i)].fAvail);
        if (i < 0 || i >= int(pl->arow.size())) return -1;
        k += step;
    }
    return OidStreamedShader97(pl->oid, i);
}

void StartEpMenu3(SW* psw)
{
    g_pfnQuadShaderAlias = IdQuadShaderAlias3;
    g_pfnQuadXf = FQuadXf3;
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
    // (debug) SLY3_EPMPROGRESS=a.b=n,..: progress paths set to n, e.g. t3_biplane_multi_sentinel.state=1 (the 2P biplane
    // dogfight's row 2026 is open once that Ep3 sentinel task (3322) has a state: it becomes available when the Ep3 hub
    // h_ext first loads)
    if (const char* sz = std::getenv("SLY3_EPMPROGRESS"))
    {
        std::string str = sz;
        for (std::size_t i = 0; i <= str.size();)
        {
            const std::size_t j = std::min(str.find(',', i), str.size());
            const std::string e = str.substr(i, j - i);
            const std::size_t k = e.find('=');
            if (k != std::string::npos)
            {
                std::vector<std::string> apath;
                for (std::size_t a = 0; a <= k;)
                {
                    const std::size_t b = std::min(e.find('.', a), k);
                    apath.push_back(e.substr(a, b - a));
                    a = b + 1;
                }
                CRef r;
                r.SetS32(std::atoi(e.c_str() + k + 1));
                FSetProgress(apath, r);
            }
            i = j + 1;
        }
    }
    s_fActive = true;
    ClearJoySteps();   // (0x1A99D0 on +0x1F0 with the pad)
    // the lists registered (their post-load 0x17AB08, run when dvd_menu loads: the open rows read the progress then)
    s_aplist = {};
    s_plistCur = nullptr;
    for (LO* q : AploOfClass(psw, 0x9D)) RegisterList(psw, static_cast<LOLIST9D3*>(q));
    // the backdrop's groups: up to the one of the last complete episode
    int iepDone = 8;
    while (iepDone >= 2 && !FAllOpen() && !FCheatAllEpisodes() && !FEpisodeComplete3(iepDone)) --iepDone;
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
    while (p.max >= 2 && !FAllOpen() && !FCheatAllEpisodes() && DtEpisode3(p.max) == 0.0f && !FEpisodeComplete3(p.max - 1)) --p.max;
    switch (s_ret.state)
    {
    case 1: p.cur = s_ret.cur; SetEpState(p, 1, -1); break;
    case 6: p.cur = s_ret.cur; p.isel = s_ret.isel; SetEpState(p, 6, -1); break;
    case 11: case 16: case 21: case 27:
    {
        // (the list's chosen row back: slot 0 for 11 / 16, 1 for 21, 2 for 27)
        p.cur = s_ret.cur;
        const size_t slot = s_ret.state == 27 ? 2 : s_ret.state == 21 ? 1 : 0;
        if (p.cur >= 0 && p.cur < 9 && s_aplist[size_t(p.cur)][slot]) s_aplist[size_t(p.cur)][slot]->isel = s_ret.item;
        SetEpState(p, s_ret.state, -1);
        break;
    }
    default: p.cur = p.max; SetEpState(p, 0, -1); break;
    }
    s_ret = EPRETURN{};   // (0x185638)
    if (std::getenv("SLY3_EPMLISTPRINT"))
    {
        auto opts = [](LO* q) {
            for (const OPT& o : q->aopt)
            {
                std::printf("   opt %d grf 0x%X:", o.eopid, o.grf);
                for (const OPTVAL& v : o.aval) std::printf(" (k %d tag 0x%X n %d g %g)", v.k, v.tagk, v.n, v.ag[0]);
                std::printf("\n");
            }
            for (const auto& [k, u] : q->afield) std::printf("   field 0x%llX = %u\n", (unsigned long long)k, u);
        };
        for (uint32_t va : {0x321730u, 0x321738u, 0x321740u, 0x321748u, 0x321750u, 0x321758u, 0x321760u})
            std::printf("elfstr 0x%X '%s'\n", va, g_disc.StrElf(va).c_str());
        for (int id : {127, 128, 129, 130, 131, 132, 494, 498, 5596, 5595})
            std::printf("text %d '%s'\n", id, StrText3(psw, id).c_str());
        for (LO* q : AploOfClass(psw, 0x9D))
        {
            LOLIST9D3* pl = static_cast<LOLIST9D3*>(q);
            std::printf("list9D oid %d mode %d ep %d rows %zu\n", q->oid, pl->mode, pl->iep, pl->arow.size());
            opts(q);
            for (auto& r : pl->arow)
            {
                LO* pn = pl->mode == 0 ? PloFindSwNearest(q, r.n) : nullptr;
                std::printf("  row kind %d n %d node %p cid %d\n", r.kind, r.n, (void*)pn, pn ? pn->Cid() : -1);
                if (pn) opts(pn);
            }
        }
    }
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
    // the lists (Two Player 11..15, Jobs && Challenges 16..20, Gallery 21..26, Bonus Movies 27..31). Natively the Two
    // Player list (episode 0's slot 0) is ported; the others are closed on the page (FEntryOpen) and only go back.
    case 11: case 13: case 14: if (!busy(idPart)) SetEpState(p, 12, -1); break;
    case 16: case 18: case 19: if (!busy(idPart)) SetEpState(p, 17, -1); break;
    case 21: case 23: case 24: if (!busy(idPart)) SetEpState(p, 22, -1); break;
    case 27: case 29: case 30: if (!busy(idPart)) SetEpState(p, 28, -1); break;
    case 12:   // the Two Player list: X starts the game shown, back to the wheel, up / down the open games
    {
        if (TakeBtn(BtnMenuConfirm()))
        {
            LO* pnode = PnodeCur(p.cur);
            if (!pnode) break;
            s_ret = EPRETURN{11, p.cur, 0, s_plistCur ? s_plistCur->isel : 0};
            hackmp3::NoteProgress(FEpisodeComplete3(sly3::EP_Holland));   // (hackmp3.h: the original's player 2 after Episode 3)
            // (PC, copsmap3.h: Cops and Robbers on the hub chosen in the map select; Venice: the original)
            if (!cops3::FStartChosen(psw, pnode)) StartChallenge3(psw, pnode, 0, -1);   // (0x283BB0(0x4BC230, node, 0, -1))
            break;
        }
        if (TakeBtn(BtnMenuBack())) { SetEpState(p, 15, 1); break; }
        // (PC, duel3.h: the Biplane Duel's players: Left / Right while it is shown; 2 = the original duel)
        if (duel3::FDuelNode(PnodeCur(p.cur)))
            if (const int d = NStepLeftRight(); d != 0)
            {
                duel3::StepPlayersChosen(d);
                PsndStartSound(0x35);
                break;
            }
        // (PC, galleon3.h: the Galleon Duel's players: Left / Right while it is shown; 2 = the original duel)
        if (galleon3::FGalleonNode(PnodeCur(p.cur)))
            if (const int d = NStepLeftRight(); d != 0)
            {
                galleon3::StepPlayersChosen(d);
                PsndStartSound(0x35);
                break;
            }
        // (PC, hackmp3.h: the Hackathon's players: Left / Right while it is shown; 2 = the original game)
        if (hackmp3::FHackNode(PnodeCur(p.cur)))
            if (const int d = NStepLeftRight(); d != 0)
            {
                hackmp3::StepPlayersChosen(d);
                PsndStartSound(0x35);
                break;
            }
        // (PC, copsmap3.h: the map select: Left / Right while Cops and Robbers is shown)
        if (cops3::FCopsNode(PnodeCur(p.cur)))
            if (const int d = NStepLeftRight(); d != 0)
            {
                cops3::StepHubChosen(d);
                PsndStartSound(0x35);
                break;
            }
        if (!s_plistCur) break;
        const int n = NStepUpDown();
        if (n == 0) break;
        if ((n < 0 && FListPrev(s_plistCur)) || (n > 0 && FListNext(s_plistCur))) SetEpState(p, n < 0 ? 13 : 14, -1);
        else Refused();
        break;
    }
    case 17:   // Jobs && Challenges: X replays the job / starts the challenge shown, up / down the open rows
    {
        if (TakeBtn(BtnMenuConfirm()))
        {
            LO* pnode = PnodeCur(p.cur);
            if (!pnode) break;
            s_ret = EPRETURN{16, p.cur, 0, s_plistCur ? s_plistCur->isel : 0};
            StartChallenge3(psw, pnode, 0, 0);   // (0x283BB0(0x4BC230, node, 0, 0))
            break;
        }
        // (Square: the "... in 3-D" start, 0x283BB0(.., 1) for a row with +0x54: the 3-D answer 1 (GAME +0x14A4), the job's
        // worlds in the 3-D mode; offered only with the 3D Mode setting on: gauge3.h FAsk3d3)
        if (FAsk3d3() && TakeBtn(0x80))
        {
            LO* pnode = PnodeCur(p.cur);
            if (!pnode || !pnode->U32Field3(0, 0x54, 0)) break;
            s_ret = EPRETURN{16, p.cur, 0, s_plistCur ? s_plistCur->isel : 0};
            StartChallenge3(psw, pnode, 0, 1);
            break;
        }
        if (TakeBtn(BtnMenuBack())) { SetEpState(p, 20, 6); break; }
        if (!s_plistCur) break;
        const int n = NStepUpDown();
        if (n == 0) break;
        if ((n < 0 && FListPrev(s_plistCur)) || (n > 0 && FListNext(s_plistCur))) SetEpState(p, n < 0 ? 18 : 19, -1);
        else Refused();
        break;
    }
    case 22:   // the Gallery: X the picture full screen, left / right the open pictures
    {
        if (TakeBtn(BtnMenuConfirm())) { SetEpState(p, 25, 26); break; }
        if (TakeBtn(BtnMenuBack())) { SetEpState(p, 25, 6); break; }
        if (!s_plistCur) break;
        const int n = NStepLeftRight();
        if (n == 0) break;
        if ((n < 0 && FListPrev(s_plistCur)) || (n > 0 && FListNext(s_plistCur))) SetEpState(p, n < 0 ? 23 : 24, -1);
        else Refused();
        break;
    }
    case 26:   // the picture: Triangle back, X / Square held zoom in / out (1..2), the left stick pans (0x262990 on 0x64E)
    {
        if (TakeBtn(BtnMenuBack())) { SetEpState(p, 21, -1); break; }
        const uint32_t grf = GrfbtnHeld();
        const float dt = g_clock.dtReal;
        if (grf & BtnMenuConfirm()) p.zoom += dt * 0.5f;
        else if (grf & 0x80) p.zoom -= dt * 0.5f;
        p.zoom = std::clamp(p.zoom, 1.0f, 2.0f);
        const float lim = 1.0f - 1.0f / p.zoom;
        p.xPan = std::clamp(p.xPan + g_joy.xLeft * dt * 0.5f, -lim, lim);
        p.yPan = std::clamp(p.yPan + g_joy.yLeft * dt * 0.5f, -lim, lim);
        break;
    }
    case 28:   // the Bonus Movies: X plays the movie shown (then the chooser again on it), left / right the movies
    {
        if (TakeBtn(BtnMenuConfirm()))
        {
            const LOLIST9D3* pl = s_plistCur;
            if (!pl || pl->isel < 0 || pl->isel >= int(pl->arow.size()) || !pl->arow[size_t(pl->isel)].fAvail) { Refused(); break; }
            s_ret = EPRETURN{27, p.cur, 0, pl->isel};
            QueueMovie(pl->arow[size_t(pl->isel)].abKey.data());   // (0x1DBD08 with the row's key)
            RequestWorldWarp(psw, 0, sly3::POINT_DEFAULT, -1);       // (0x289070(GAME, 0, 0x1A8, 0, -1))
            break;
        }
        if (TakeBtn(BtnMenuBack())) { SetEpState(p, 31, 6); break; }
        if (!s_plistCur) break;
        const int n = NStepLeftRight();
        if (n == 0) break;
        if ((n < 0 && FListPrev(s_plistCur)) || (n > 0 && FListNext(s_plistCur))) SetEpState(p, n < 0 ? 29 : 30, -1);
        else Refused();
        break;
    }
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
        if (id >= 1481 && id <= 1498)
        {
            // the lists' strings (orig3 0x17C9B0..0x17CDE0 on the chosen row's object, 0x17C6C8): 1484 / 1489 its name
            // (+0x40), 1485 / 1490 its description (+0x44), 1486 / 1491 what X (and Square) do, 1488 its kind (1 a
            // completed job, 0 a challenge). (1487 / 1498: the high score / best time of a done challenge or of the
            // hacking game 1815: challenge3.cpp's strings.)
            pstr->clear();
            const LO* pnode = FEpMenu3Active() ? PnodeCur(p.cur) : nullptr;
            if (!pnode) return true;
            const int mode = int(pnode->U32Field3(0, 0x50, 0));
            const bool fFlags = pnode->U32Field3(0, 0x54, 0) != 0;
            switch (id)
            {
            case 1484: case 1489:
                if (const int idName = int(pnode->U32Field3(0, 0x40, 0xFFFFFFFFu)); idName != -1) *pstr = text(idName);
                break;
            case 1485: case 1490:
                if (duel3::FDescription(pnode, pstr)) break;   // (PC, duel3.h: the free-for-all's, its pilots "Player N")
                if (galleon3::FDescription(pnode, pstr)) break;   // (PC, galleon3.h: the free-for-all's)
                *pstr = text(int(pnode->U32Field3(0, 0x44, 0xFFFFFFFFu)));
                break;
            case 1486: case 1491:   // ("%s : &2X&." and, with the object's +0x54, "\n%s : &2S&.")
                *pstr = text(mode == 1 ? 128 : 131) + " : " + SzMenuConfirmGlyph();
                // (the "... in 3-D : Square" line of a 3-D job; only with the 3D Mode setting on: gauge3.h FAsk3d3)
                if (fFlags && FAsk3d3()) *pstr += "\n" + text(mode == 1 ? 129 : 132) + " : &2S&.";
                break;
            case 1488:
                if (mode == 1) *pstr = text(127);
                else if (mode == 0) *pstr = text(130);
                break;
            default: break;
            }
            return true;
        }
        return false;
    }
}

LO* PnodeEpMenuCur3() { return FEpMenu3Active() ? PnodeCur(s_epm3.cur) : nullptr; }

LO* PnodeTwoPlayerShown3()
{
    // (the Two Player list's page: state 12 of episode 0)
    if (!FEpMenu3Active() || s_epm3.state != 12 || s_epm3.cur != 0) return nullptr;
    return PnodeCur(s_epm3.cur);
}

std::string StrEpisodeName3(int iep)
{
    return iep >= 0 && iep < 9 && s_pswEpm ? StrText3(s_pswEpm, NElf(VA_NAMES + 4 * uint32_t(iep))) : std::string(sly3::SzEpisode(iep));
}
