// Sly 3's score tally (class 0xA5, e.g. h_hack's 3335: "Level Score / Total Score / High Score"), ported from the Sly 3
// decomp: the update 0x19FC80, the state 0x19FF00, the strings 0x1A01F8 (script strings 1010, 1011, 1013..1035; 1012 is
// the high score key's "%d", 0x25F018 on SW +0x42C8 "hack_multi" "nHighScore"), the show 0x1A0130 (state 1) / hide
// 0x1A0160 (state 9), the scripts' methods (EOPIDs 2034..2038 / 2043: 0x111B80 / 0x111C00 / 0x111CB8 / 0x111D68 /
// 0x111E30 / 0x111EA8 -> 0x1A03F8 / 0x1A0480 / 0x1A04E0 / 0x1A0548 / 0x1A0580 / 0x1A0588) and its fields (2041 +0x128 the
// record check, 2042 +0x12C its title, 2039 +0x134 the score, 2040 +0x138 the total).
//
// The script fills the rows (each: the part that brings it in, the part that takes it out, its name, its points) and
// shows the object; the tally plays each row (its points counted up over 2 s, X / a face button skips), adds the score
// to the total (part 0x40D), a new record (0x40C: written to the key, one of the record lines spoken), then waits for a
// face button and hides itself.
//
// Also the ship battles' stats on 1945 (p_at_sea): 1964 the rank (0x21A818: (2 x captured + sunk) / 10 -> texts 501..510),
// 1965 / 1966 the sunk / captured counts (0x25F018 on SW +0x42C0 / +0x42C4); 1960..1963 (0x21A7B8: the battle
// manager's 0x468D24 entries +0x70, "%d" of each row's reward while index < count +0x94) are game/ship3.cpp's
// (StrBattleValue, the rows of TallyBattle 0x21A300).

#include "tally3.h"

#include "brx.h"
#include "card3.h"
#include "clock.h"
#include "disc.h"
#include "frontend.h"
#include "lo.h"
#include "menu.h"
#include "objmethod.h"
#include "ref.h"
#include "savedata.h"
#include "screen3.h"
#include "screenrt.h"
#include "sound.h"
#include "sw.h"
#include "game/sly3.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

int NRandInRange(int nLow, int nHigh);   // util.h (0x11D3F8 / orig3 0x11E8F0)
void PauseGame(int grf);    // ai.h (0x1A93C8 / 0x1A94A8 natively Sly 2's count)
void ResumeGame(int grf);

namespace
{
struct TALLY3
{
    int state = 0;                  // +0x120
    float tState = 0.0f;            // +0x124
    int from = 0, to = 0, total = 0;// +0x130 / +0x134 / +0x138 (the fields' values at the first show)
    bool fFields = false;
    std::array<std::array<int, 5>, 2> aStat{}, aStatAll{};   // +0x13C / +0x150 (group 0 / 1), +0x164 / +0x178
    struct ROW { int partIn = -1, partOut = -1, idName = -1, value = 0; };
    std::vector<ROW> arow;          // +0x18C.. (8 at most), +0x20C the count
    int irow = 0;                   // +0x210
    std::vector<int> aidPick;       // +0x214.. (4 at most), +0x224 the count
    int idPick = -1;                // +0x228
};
std::unordered_map<LO*, TALLY3> s_mptally;
uint32_t s_serial = 0;
SW* s_psw = nullptr;

bool FPrint() { return std::getenv("SLY3_TALLYPRINT") != nullptr; }

TALLY3& Tally(LO* plo)
{
    if (g_psw && s_serial != g_psw->serial)
    {
        s_mptally.clear();
        s_serial = g_psw->serial;
    }
    TALLY3& t = s_mptally[plo];
    if (!t.fFields && plo)
    {
        // (the options' fields: natively stored as the loader read them)
        t.from = int(plo->U32Field3(0, 0x130, 0));
        t.to = int(plo->U32Field3(0, 0x134, 0));
        t.total = int(plo->U32Field3(0, 0x138, 0));
        t.fFields = true;
    }
    return t;
}

bool FRecordCheck(const LO* plo) { return plo->U32Field3(0, 0x128, 0) != 0; }
int IdTitle(const LO* plo) { return int(plo->U32Field3(0, 0x12C, 0xFFFFFFFFu)); }

const std::vector<std::string> c_apathHigh = {"hack_multi", "nHighScore"};
int NRefT(const CRef& r) { return r.m_tagk == TAGK_S32 ? r.m_n : r.m_tagk == TAGK_F32 ? int(r.m_g) : r.m_tagk == TAGK_Bool ? int(r.m_bool) : 0; }

// orig3 0x001A01A0: no card dialog / menu up and a face button pressed (0xF0)
bool FSkip() { return !FCardMessageShown3() && !FMenuShown() && GrfbtnTakePressed(0xF0) != 0; }

// orig3: 0x0019FF00
void SetTallyState(LO* plo, TALLY3& t, int k)
{
    const int old = t.state;
    if (k == old) return;
    if (old == 0) PauseGame(2);   // (0x1A93C8(SW, -1, 2), 0x1A94A8)
    t.state = k;
    t.tState = g_clock.t;
    if (FPrint()) std::printf("tally3: %d state %d -> %d (row %d of %zu, %d -> %d, total %d)\n", plo->oid, old, k, t.irow, t.arow.size(), t.from, t.to, t.total);
    auto row = [&]() -> const TALLY3::ROW* { return t.irow >= 0 && t.irow < int(t.arow.size()) ? &t.arow[size_t(t.irow)] : nullptr; };
    switch (k)
    {
    case 1: t.irow = 0; break;
    case 5: ++t.irow; break;
    case 2: if (const TALLY3::ROW* r = row()) SelectScreenObjPart(plo, r->partIn); break;
    case 3:
        t.from = t.to;
        if (const TALLY3::ROW* r = row()) t.to += r->value;
        break;
    case 4: if (const TALLY3::ROW* r = row()) SelectScreenObjPart(plo, r->partOut); break;
    case 6:
        t.total += t.to;
        SelectScreenObjPart(plo, 0x40D);
        break;
    case 7:
    {
        CRef r;
        r.SetS32(t.total);
        FSetProgress(c_apathHigh, r);
        t.idPick = t.aidPick.empty() ? -1 : t.aidPick[size_t(NRandInRange(0, int(t.aidPick.size()) - 1))];
        if (t.idPick != -1) PsndStartDialog(t.idPick);   // (0x2485D8(D_00474FF0, pick, 1, 0))
        SelectScreenObjPart(plo, 0x40C);
        break;
    }
    case 0: ResumeGame(2); break;   // (0x1A9420, 0x1A94F0)
    default: break;   // (9: 0x13BC80 on the object: its base hide follows)
    }
}

// orig3: 0x0019FC80 (each frame; the show / hide overrides 0x1A0130 / 0x1A0160 as the object's own show / hide)
void UpdateTally(LO* plo)
{
    TALLY3& t = Tally(plo);
    const int nScreen = NScreenObjState(plo);
    if (t.state == 0)
    {
        if (nScreen >= 2) SetTallyState(plo, t, 1);   // (shown: 0x1A0130)
        else return;
    }
    else if (t.state != 9 && nScreen < 2 && FScreenObjDone(plo))
        SetTallyState(plo, t, 9);   // (hidden from outside: 0x1A0160)
    for (int guard = 0; guard < 16; ++guard)
    {
        const int st = t.state;
        const int c = int(t.arow.size());
        auto busy = [&](int part) { return part != -1 && FScreenObjBusy(plo, part); };
        switch (st)
        {
        case 1:
            if (nScreen != 3) SetTallyState(plo, t, t.irow < c ? 2 : 6);   // (+0x48: not in its show part)
            break;
        case 2:
            if (!busy(t.arow[size_t(t.irow)].partIn)) SetTallyState(plo, t, 3);
            break;
        case 3:
            if (g_clock.t - t.tState > 2.0f || FSkip()) SetTallyState(plo, t, t.irow + 1 < c ? 4 : 6);
            break;
        case 4:
            if (!busy(t.arow[size_t(t.irow)].partOut)) SetTallyState(plo, t, 5);
            break;
        case 5:
            SetTallyState(plo, t, t.irow < c ? 2 : 6);
            break;
        case 6:
            if (!busy(0x40D))
                SetTallyState(plo, t, FRecordCheck(plo) && NRefT(RefGetProgress(c_apathHigh)) < t.total ? 7 : 8);
            break;
        case 7:
            if (!busy(0x40C) && (t.idPick == -1 || !FDialogPlaying(t.idPick))) SetTallyState(plo, t, 8);
            break;
        case 8:
            if (FSkip())
            {
                SetTallyState(plo, t, 9);   // (vt slot 54: 0x1A0160, then the base hide)
                HideScreenObj(plo);
            }
            break;
        case 9:
            if (FScreenObjDone(plo)) SetTallyState(plo, t, 0);
            break;
        default: break;
        }
        if (t.state == st) break;
    }
}

struct INITTALLY3
{
    INITTALLY3()
    {
        // the methods (orig3 0x111B80.. -> 0x1A03F8 ..)
        RegisterObjectMethod3(0x111B80, [](LO* plo, const std::vector<CRef>& a) {   // 2034: the tables cleared
            TALLY3& t = Tally(plo);
            for (auto& g : t.aStat) g.fill(0);
            if (NArg(a, 0, 0)) for (auto& g : t.aStatAll) g.fill(0);
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x111C00, [](LO* plo, const std::vector<CRef>& a) {   // 2035: (group, column, points) added
            TALLY3& t = Tally(plo);
            const int g = NArg(a, 0, -1), k = NArg(a, 1, -1), n = NArg(a, 2, 0);
            if (k != -1 && k < 5 && unsigned(g) < 2u)
            {
                t.aStat[size_t(g)][size_t(k)] += n;
                t.aStatAll[size_t(g)][size_t(k)] += n;
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x111CB8, [](LO* plo, const std::vector<CRef>& a) {   // 2036: a table's value
            TALLY3& t = Tally(plo);
            const int g = NArg(a, 0, -1), k = NArg(a, 1, -1);
            const bool fAll = NArg(a, 2, 0) != 0;
            CRef r;
            r.SetS32(k == -1 || k >= 5 || unsigned(g) >= 2u ? 0 : (fAll ? t.aStatAll : t.aStat)[size_t(g)][size_t(k)]);
            return r;
        });
        RegisterObjectMethod3(0x111D68, [](LO* plo, const std::vector<CRef>& a) {   // 2037: a row (parts, name, points)
            TALLY3& t = Tally(plo);
            if (t.arow.size() < 8) t.arow.push_back({NArg(a, 0, -1), NArg(a, 1, -1), NArg(a, 2, -1), NArg(a, 3, 0)});
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x111E30, [](LO* plo, const std::vector<CRef>&) {     // 2038: the rows cleared (0x1A0580)
            Tally(plo).arow.clear();
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod3(0x111EA8, [](LO* plo, const std::vector<CRef>& a) {   // 2043: a record line (a dialog)
            TALLY3& t = Tally(plo);
            if (t.aidPick.size() < 4) t.aidPick.push_back(NArg(a, 0, -1));
            return CRef(TAGK_Void);
        });
        // the strings (orig3 0x1A01F8 on the drawn object: natively the world's tally, one per world)
        auto tally = []() -> std::pair<LO*, TALLY3*> {
            for (auto& [plo, t] : s_mptally)
                if (plo && g_psw && s_serial == g_psw->serial) return {plo, &t};
            return {nullptr, nullptr};
        };
        auto num = [](int v) { return std::to_string(v); };
        SetScriptString3(1015, [tally] {   // 0x3F7: its title
            auto [plo, t] = tally();
            return plo && IdTitle(plo) != -1 && s_psw ? StrText3(s_psw, IdTitle(plo)) : std::string();
        });
        SetScriptString3(1010, [tally, num] {   // 0x3F2: the score, counted up over 2 s in state 3
            auto [plo, t] = tally();
            if (!t) return std::string();
            int v = t->to;
            if (t->state == 3 && t->from != t->to)
            {
                const float u = std::min(1.0f, std::max(0.0f, (g_clock.t - t->tState) * 0.5f));
                const int a = t->from, b = t->to;
                v = a < b ? a + int(u * (float(b) + 0.999f - float(a))) : b + int((1.0f - u) * (float(a) + 0.999f - float(b)));
            }
            return num(v);
        });
        SetScriptString3(1011, [tally, num] { auto [plo, t] = tally(); return t ? num(t->total) : std::string(); });   // 0x3F3
        SetScriptString3(1012, [num] {   // the high score key (0x25F018)
            const CRef r = RefGetProgress(c_apathHigh);
            return r.m_tagk == TAGK_Nil ? std::string() : num(NRefT(r));
        });
        SetScriptString3(1013, [tally] {   // 0x3F5: the row's name
            auto [plo, t] = tally();
            if (!t || t->irow < 0 || t->irow >= int(t->arow.size()) || !s_psw) return std::string();
            return StrText3(s_psw, t->arow[size_t(t->irow)].idName);
        });
        SetScriptString3(1014, [tally, num] {   // 0x3F6: the row's points
            auto [plo, t] = tally();
            if (!t || t->irow < 0 || t->irow >= int(t->arow.size())) return std::string();
            return num(t->arow[size_t(t->irow)].value);
        });
        for (int i = 0; i < 20; ++i)   // 0x3F8.. group 0, its totals 0x3FD.., group 1 0x402.., its totals 0x407..
            SetScriptString3(1016 + i, [tally, num, i] {
                auto [plo, t] = tally();
                if (!t) return std::string();
                const int g = i / 5, k = i % 5;
                switch (g)
                {
                case 0: return num(t->aStat[0][size_t(k)]);
                case 1: return num(t->aStatAll[0][size_t(k)]);
                case 2: return num(t->aStat[1][size_t(k)]);
                default: return num(t->aStatAll[1][size_t(k)]);
                }
            });
        // 1945's ship battle stats (p_at_sea)
        SetScriptString3(1964, [] {   // orig3 0x21A818: the rank
            const int captured = NRefT(RefGetProgress({"battle", "cCaptured"}));
            const int sunk = NRefT(RefGetProgress({"battle", "cSunk"}));
            int level = (captured * 2 + sunk) / 10;
            if (level > 8) level = 9;
            if (level < 0) level = 0;
            return s_psw ? StrText3(s_psw, int(g_disc.U32Elf(0x325E00 + 4u * uint32_t(level)))) : std::string();
        });
        SetScriptString3(1965, [num] {   // 0x25F018 on SW +0x42C0
            const CRef r = RefGetProgress({"battle", "cSunk"});
            return r.m_tagk == TAGK_Nil ? std::string() : num(NRefT(r));
        });
        SetScriptString3(1966, [num] {   // 0x25F018 on SW +0x42C4
            const CRef r = RefGetProgress({"battle", "cCaptured"});
            return r.m_tagk == TAGK_Nil ? std::string() : num(NRefT(r));
        });
    }
} s_inittally3;
}  // namespace

// each frame (frontend.cpp): the world's tallies
void UpdateTallies3(SW* psw)
{
    s_psw = psw;
    if (!psw) return;
    for (LO* plo : psw->mpcidaplo[0xA5])
        if (plo && !plo->fRemoved) UpdateTally(plo);
}
