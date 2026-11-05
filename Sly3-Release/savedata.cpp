#include "savedata.h"

#include "sw.h"
#include "transition.h"
#include "task.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

GAMESAVE g_gamesave;

namespace
{
// a trie node (10 bytes in the world file)
struct PENT
{
    int16_t ch;       // the path element's symbol
    int8_t kind;      // -1 inner node; 0, 1, 2, 9, 10 S32, 3 F32, 4 bool (bits when indexed), 5 VECTOR, 6 4 x S32, 7 2 x S32, 8 3 x S32
    uint8_t fIndexed; // the next path element indexes it
    uint16_t ib;      // the value's offset in the save
    uint16_t iChild, iSibling;
};

struct LAYOUT
{
    std::vector<std::pair<int, std::string>> aname;
    std::vector<PENT> aent;
    std::vector<uint8_t> abRecords;
} s_layout;

// orig 0x1D8640 (h * 33 + toupper)
uint32_t HashName(const std::string& s)
{
    uint32_t h = 0;
    for (unsigned char c : s) h = h * 33 + static_cast<uint32_t>(std::toupper(c));
    return h;
}

// orig 0x1D8720 / 0x233800: a path element's symbol (a name, else a number)
int SymFromName(const std::string& s)
{
    const uint32_t h = HashName(s);
    for (const auto& [n, name] : s_layout.aname)
        if (HashName(name) == h && name == s) return n;
    return static_cast<int16_t>(std::atoi(s.c_str()));
}

// orig 0x1D7DA0 (the key: the path's symbols, as an option of kind 22 keeps them)
bool FFindEntryKey(const std::vector<int>& akey, const PENT** ppe, int* pn)
{
    const std::vector<PENT>& ae = s_layout.aent;
    if (ae.empty() || ae[0].iChild == 0 || akey.empty()) return false;
    const PENT* pe = &ae[ae[0].iChild];
    *pn = 0;
    std::size_t i = 0;
    for (int guard = 0; guard < 100000; ++guard)
    {
        if (pe->ch == static_cast<int16_t>(akey[i]))
        {
            ++i;
            if (i == akey.size()) { *ppe = pe; return true; }
            if (pe->fIndexed)
            {
                *ppe = pe;
                *pn = akey[i++];
                return i == akey.size();
            }
            if (pe->kind != -1 || !pe->iChild || pe->iChild >= ae.size()) return false;
            pe = &ae[pe->iChild];
        }
        else
        {
            if (!pe->iSibling || pe->iSibling >= ae.size()) return false;
            pe = &ae[pe->iSibling];
        }
    }
    return false;
}

bool FFindEntry(const std::vector<std::string>& apath, const PENT** ppe, int* pn)
{
    std::vector<int> akey;
    for (const std::string& s : apath) akey.push_back(SymFromName(s));
    return FFindEntryKey(akey, ppe, pn);
}

template <typename T> T Rd(std::size_t ib)
{
    T v{};
    if (ib + sizeof(T) <= CB_PROGRESS) std::memcpy(&v, g_gamesave.ab + ib, sizeof(T));
    return v;
}
template <typename T> void Wr(std::size_t ib, const T& v)
{
    if (ib + sizeof(T) <= CB_PROGRESS) std::memcpy(g_gamesave.ab + ib, &v, sizeof(T));
}
}

void SetProgressLayout(const PROGRESS& progress)
{
    s_layout.aname = progress.aname;
    if (std::getenv("SLY2N_SYMNAMES"))  // debug: the progress symbols (a task's is its oid)
        for (const auto& [n, name] : progress.aname) std::printf("sym %d %s\n", n, name.c_str());
    s_layout.abRecords = progress.abRecords;
    s_layout.aent.resize(progress.abRecords.size() / 10);
    for (std::size_t i = 0; i < s_layout.aent.size(); ++i)
    {
        const uint8_t* p = progress.abRecords.data() + i * 10;
        PENT& e = s_layout.aent[i];
        std::memcpy(&e.ch, p, 2);
        e.kind = static_cast<int8_t>(p[2]);
        e.fIndexed = p[3];
        std::memcpy(&e.ib, p + 4, 2);
        std::memcpy(&e.iChild, p + 6, 2);
        std::memcpy(&e.iSibling, p + 8, 2);
    }
}

// orig3: 0x21C0F0 ValidateGameSave (Sly 2 0x1D7C80; the same code with the Sly 3 sizes)
void ValidateGameSave()
{
    const std::vector<uint8_t>& ab = s_layout.abRecords;
    if (ab.size() < 10) return;
    int32_t sum = 0;
    for (uint8_t b : ab) sum += b;
    uint16_t n16;
    std::memcpy(&n16, ab.data() + ab.size() - 6, 2);
    const int32_t n = n16;
    int32_t nSave, sumSave;
    std::memcpy(&nSave, g_gamesave.ab + CB_PROGRESS, 4);
    std::memcpy(&sumSave, g_gamesave.ab + CB_PROGRESS + 4, 4);
    if (n == nSave && sum == sumSave) return;
    std::memset(g_gamesave.ab, 0, CB_PROGRESS);
    std::memcpy(g_gamesave.ab + CB_PROGRESS, &n, 4);
    std::memcpy(g_gamesave.ab + CB_PROGRESS + 4, &sum, 4);
}

namespace
{
CRef RefGetEntry(const PENT* pe, int n)
{
    CRef ref;
    const std::size_t ib = pe->ib;
    switch (pe->kind)
    {
    case 0: case 1: case 2: case 10: case 9: ref.SetS32(Rd<int32_t>(ib + 4u * n)); break;
    case 3: ref.SetF32(Rd<float>(ib + 4u * n)); break;
    case 4:
        if (pe->fIndexed) ref.SetBool((Rd<uint8_t>(ib + n / 8) >> (n % 8)) & 1);
        else ref.SetBool(Rd<uint8_t>(ib) != 0);
        break;
    case 5: ref.SetVector(glm::vec3(Rd<float>(ib + 16u * n), Rd<float>(ib + 16u * n + 4), Rd<float>(ib + 16u * n + 8))); break;
    default: ref.SetS32(Rd<int32_t>(ib + 16u * n)); break;  // 6/7/8: TODO(splice): the 2/3/4-int forms
    }
    return ref;
}

bool FSetEntry(const PENT* pe, int n, const CRef& ref)
{
    const std::size_t ib = pe->ib;
    switch (pe->kind)
    {
    case 0: case 1: case 2: case 9: case 10: Wr<int32_t>(ib + 4u * n, ref.RefCoerceS32().m_n); return true;
    case 3: Wr<float>(ib + 4u * n, ref.m_tagk == TAGK_F32 ? ref.m_g : static_cast<float>(ref.m_n)); return true;
    case 4:
    {
        const bool f = ref.m_tagk == TAGK_Bool ? ref.m_bool != 0 : ref.m_n != 0;
        if (pe->fIndexed)
        {
            uint8_t b = Rd<uint8_t>(ib + n / 8);
            b = f ? (b | (1u << (n % 8))) : (b & ~(1u << (n % 8)));
            Wr<uint8_t>(ib + n / 8, b);
        }
        else Wr<uint8_t>(ib, f ? 1 : 0);
        return true;
    }
    case 5:
    {
        const glm::vec3 v = ref.GetVector();
        Wr<float>(ib + 16u * n, v.x);
        Wr<float>(ib + 16u * n + 4, v.y);
        Wr<float>(ib + 16u * n + 8, v.z);
        return true;
    }
    default: return false;
    }
}

std::vector<int> AkeyFromS16(const std::vector<int16_t>& an) { return std::vector<int>(an.begin(), an.end()); }
}

// orig 0x001D7ED8
CRef RefGetProgress(const std::vector<std::string>& apath)
{
    const PENT* pe = nullptr;
    int n = 0;
    if (!FFindEntry(apath, &pe, &n) || pe->kind == -1) return CRef();
    return RefGetEntry(pe, n);
}

CRef RefGetProgressKey(const std::vector<int16_t>& akey)
{
    const PENT* pe = nullptr;
    int n = 0;
    if (!FFindEntryKey(AkeyFromS16(akey), &pe, &n) || pe->kind == -1) return CRef();
    return RefGetEntry(pe, n);
}

// orig 0x001D8C10 (0x1D7E80: the address of a path's value in the save; null when the path does not resolve or the
// value is not an int / float: for the players' counters "hpCur" / "mpCur")
int32_t* PnProgressSlot(const std::vector<std::string>& apath)
{
    const PENT* pe = nullptr;
    int n = 0;
    if (!FFindEntry(apath, &pe, &n) || pe->kind == -1) return nullptr;
    switch (pe->kind)
    {
    case 0: case 1: case 2: case 3: case 9: case 10:
        return reinterpret_cast<int32_t*>(g_gamesave.ab + pe->ib + 4u * static_cast<unsigned>(n));
    default: return nullptr;
    }
}

// orig 0x001D80C8
bool FSetProgress(const std::vector<std::string>& apath, const CRef& ref)
{
    const PENT* pe = nullptr;
    int n = 0;
    if (!FFindEntry(apath, &pe, &n) || pe->kind == -1) return false;
    return FSetEntry(pe, n, ref);
}

bool FSetProgressKey(const std::vector<int16_t>& akey, const CRef& ref)
{
    const PENT* pe = nullptr;
    int n = 0;
    if (!FFindEntryKey(AkeyFromS16(akey), &pe, &n) || pe->kind == -1) return false;
    return FSetEntry(pe, n, ref);
}

bool FLoadGameSave(const std::string& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot open " + path; return false; }
    GAMESAVE save;
    in.read(reinterpret_cast<char*>(save.ab), CB_GAMESAVE);
    if (in.gcount() != CB_GAMESAVE) { error = path + " is not a Sly 3 save slot (0x6808 bytes)"; return false; }
    g_gamesave = save;
    return true;
}

bool FWriteGameSave(const std::string& path, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) { error = "cannot write " + path; return false; }
    out.write(reinterpret_cast<const char*>(g_gamesave.ab), CB_GAMESAVE);
    return static_cast<bool>(out);
}

int NSymProgress(const std::string& name) { return SymFromName(name); }

int NGamePercent(const GAMESAVE& gs)
{
    static const int aw[9] = {13, 86, 97, 147, 141, 160, 106, 182, 166};   // (0x2C2DC0)
    static const char* const asz[9] = {"intro", "nightclub", "palace", "temple", "prison", "castle", "train", "sawmill", "blimp"};
    GAMESAVE saved = g_gamesave;
    g_gamesave = gs;
    int sum = 0, total = 0;
    for (int i = 0; i < 9; ++i)
    {
        total += aw[i] * 100;
        const CRef r = RefGetProgress({"episode", asz[i], "percent_complete"});
        if (r.m_tagk == TAGK_S32) sum += r.m_n * aw[i];
    }
    total += 0xD0;
    const int k = SymFromName("fVault");
    for (const PENT& e : s_layout.aent)
    {
        if (e.kind != 4 || e.ch != k) continue;
        const uint8_t* pv = gs.ab + e.ib;
        if (e.fIndexed)
        {
            for (int j = 0; j < e.fIndexed; ++j)
                if (e.ib + j / 8 < CB_GAMESAVE && (pv[j / 8] & (1 << (j % 8)))) sum += 0x1A;
        }
        else if (e.ib < CB_GAMESAVE && *pv) sum += 0x1A;
    }
    g_gamesave = saved;
    if (sum == 0) return 0;
    if (sum == total) return 100;
    return std::max(1, std::min(sum * 100 / total, 99));
}

// orig: 0x00161478 func_00161478 (UpdateTasks, from UpdateClock 0x14D0B8 when not paused): the play times
void UpdateGameTimers(float dt)
{
    const CRef r = RefGetProgress({"dt"});
    if (r.m_tagk == TAGK_F32)
    {
        CRef w;
        w.SetF32(r.m_g + dt);
        FSetProgress({"dt"}, w);
    }
    // the current episode's (SW +0x3B58[i]: "episode" <name> "dt")
    static const char* const s_asz[9] = {"intro", "nightclub", "palace", "temple", "prison", "castle", "train", "sawmill", "blimp"};
    const unsigned i = static_cast<unsigned>(IepisodeCur());
    if (i < 9)
    {
        const CRef re = RefGetProgress({"episode", s_asz[i], "dt"});
        if (re.m_tagk == TAGK_F32)
        {
            CRef w;
            w.SetF32(re.m_g + dt);
            FSetProgress({"episode", s_asz[i], "dt"}, w);
        }
    }
    // the current job's time (SW +0x3B0C +0xA4 while JOY +0x1020, the seconds since the pad was used, is under 10)
    UpdateJobTimer(dt);
}

// orig: 0x001D84B0 func_001D84B0: the record copied for a card slot, every "from_memcard" value set (the tasks
// loaded from a card know it)
void CopyGameSaveForSlot(GAMESAVE& gs)
{
    gs = g_gamesave;
    int k = -1;
    for (const auto& [n, name] : s_layout.aname)
        if (name == "from_memcard") k = n;
    if (k == -1) return;
    for (const PENT& e : s_layout.aent)
        if (e.ch == k && e.ib < CB_GAMESAVE) gs.ab[e.ib] = 1;
}

bool FWriteGameSaveBlock(const std::string& path, const GAMESAVE& gs, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) { error = "cannot write " + path; return false; }
    out.write(reinterpret_cast<const char*>(gs.ab), CB_GAMESAVE);
    return static_cast<bool>(out);
}

std::string StrProgressKey(const std::vector<int16_t>& akey)
{
    std::string str;
    for (int16_t n : akey)
    {
        std::string name = std::to_string(n);
        for (const auto& [k, sz] : s_layout.aname)
            if (k == n) name = sz;
        str += (str.empty() ? "" : ".") + name;
    }
    return str;
}

// (QoL Replay Jobs: savedata.h)
void MergeProgressKeepDone(GAMESAVE& gs, const GAMESAVE& gsKeep)
{
    const std::vector<PENT>& ae = s_layout.aent;
    if (ae.empty() || ae[0].iChild == 0 || ae[0].iChild >= ae.size()) return;
    auto name = [](int16_t ch) -> std::string {
        for (const auto& [n, str] : s_layout.aname)
            if (static_cast<int16_t>(n) == ch) return str;
        return std::string();
    };
    auto rd32 = [](const GAMESAVE& g, size_t ib) { int32_t n = 0; if (ib + 4 <= CB_PROGRESS) std::memcpy(&n, g.ab + ib, 4); return n; };
    auto wr32 = [](GAMESAVE& g, size_t ib, int32_t n) { if (ib + 4 <= CB_PROGRESS) std::memcpy(g.ab + ib, &n, 4); };
    auto fInt = [](const PENT& e) { return !e.fIndexed && (e.kind == 0 || e.kind == 1 || e.kind == 2 || e.kind == 9 || e.kind == 10); };
    auto maxInt = [&](const PENT& e) { if (fInt(e)) wr32(gs, e.ib, std::max(rd32(gs, e.ib), rd32(gsKeep, e.ib))); };
    auto keepInt = [&](const PENT& e) { if (fInt(e)) wr32(gs, e.ib, rd32(gsKeep, e.ib)); };
    auto orBool = [&](const PENT& e) { if (e.kind == 4 && !e.fIndexed && e.ib < CB_PROGRESS) gs.ab[e.ib] = (gs.ab[e.ib] || gsKeep.ab[e.ib]) ? 1 : 0; };
    auto forChildren = [&](const PENT& e, const std::function<void(const PENT&, const std::string&)>& fn) {
        if (e.kind != -1 || e.fIndexed || !e.iChild || e.iChild >= ae.size()) return;
        for (size_t i = e.iChild, guard = 0; i && i < ae.size() && guard < ae.size(); i = ae[i].iSibling, ++guard) fn(ae[i], name(ae[i].ch));
    };
    for (size_t i = ae[0].iChild, guard = 0; i && i < ae.size() && guard < ae.size(); i = ae[i].iSibling, ++guard)
    {
        const PENT& e = ae[i];
        const std::string str = name(e.ch);
        if (str.size() > 3 && (str[0] == 't' || str[0] == 'm') && std::isdigit(static_cast<unsigned char>(str[1])) && str[2] == '_')
            forChildren(e, [&](const PENT& c, const std::string& s) {
                if (s == "state" || s == "complete_count" || s == "focus_count") maxInt(c);
                else if (s == "is_complete") orBool(c);
            });
        else if (str == "episode")
            forChildren(e, [&](const PENT& ep, const std::string&) {
                forChildren(ep, [&](const PENT& c, const std::string& s) { if (s == "percent_complete") maxInt(c); });
            });
        else if (str == "episodeBest") maxInt(e);
        else if (str == "widCurrent" || str == "episodeCurrent" || str == "oidPoCurrent") keepInt(e);
    }
}
