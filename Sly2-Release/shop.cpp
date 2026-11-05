// See shop.h. Reference: the decomp's func_001DC260.c / func_001DBD10.cpp / func_001DBBE0.c / func_001DBBA0.c /
// func_001DBEF0.c / func_001DC4B0.c / func_001DA858.c / func_001DC790.c / func_001DC820.c / func_001DC890.cpp /
// func_001DC9C0.c / func_001DCA30.c / func_001DCAB0.c / func_001DAE58.c / func_001DAEA8.c / func_00161798.c /
// func_001617E0.cpp, m_1DAC58_p3.cpp's 0x1DB870 / 0x1DAF38, m_1D93D8.cpp's 0x1DA950 / 0x1DA7D0 (Sly2-Reference).

#include "shop.h"
#include "region.h"
#include "sound.h"
#include "ai.h"

#include "clock.h"
#include "cls.h"
#include "disc.h"
#include "frontend.h"
#include "hud.h"
#include "joy.h"
#include "ref.h"
#include "savedata.h"
#include "screenrt.h"
#include "splayer.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "update.h"
#include "actor.h"
#include "gadget.h"
#include "pcinput.h"
#include "worldaccess.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace
{
int32_t NElf(uint32_t va) { return int32_t(g_disc.U32Elf(va)); }  // (an address comes back as the USA one: disc.h)
int NRef(const CRef& r) { return r.m_tagk == TAGK_S32 ? r.m_n : r.m_tagk == TAGK_F32 ? int(r.m_g) : r.m_tagk == TAGK_Bool ? int(r.m_bool) : 0; }

// the items (0x2BCD00: 0x2B of 0x20 bytes): +4 the kind (7 / 8 / 9: the character), +8 the cost, +0x10 the
// episode it opens in, +0x14 its name, +0x18 its description
int ItemKind(int i) { return NElf(0x2BCD00 + 0x20 * uint32_t(i) + 4); }
int ItemCost(int i) { return NElf(0x2BCD00 + 0x20 * uint32_t(i) + 8); }
int ItemEpisode(int i) { return NElf(0x2BCD00 + 0x20 * uint32_t(i) + 0x10); }
int ItemName(int i) { return NElf(0x2BCD00 + 0x20 * uint32_t(i) + 0x14); }
int ItemDesc(int i) { return NElf(0x2BCD00 + 0x20 * uint32_t(i) + 0x18); }
// the loot (0x2C3288: 0x40 of 0xC bytes): +0 its value, +4 its name
int LootValue(int i) { return NElf(0x2C3288 + 0xC * uint32_t(i)); }
int LootName(int i) { return NElf(0x2C3288 + 0xC * uint32_t(i) + 4); }

constexpr int CROW = 6;                 // orig D_002C35B4
constexpr float DY_ROW = 40.0f;         // orig D_002C35B0

// orig 0x00161798 / 0x001617E0: the coins
int CCoins() { return NRef(RefGetProgress({"loot", "ccoin"})); }
void SetCoins(int n) { CRef r; r.SetS32(n); FSetProgress({"loot", "ccoin"}, r); }
// orig 0x001DAE58 / 0x001DAEA8 / 0x001DAF38: an item open, owned, bought
bool FItemOpen(int i) { return ItemEpisode(i) != -1 && NRef(RefGetProgress({"episodeBest"})) >= ItemEpisode(i); }
bool FItemOwned(int i) { return NRef(RefGetProgress({"powerUps", "mppukfAcquired", std::to_string(i)})) != 0; }
int CLoot(int i) { return NRef(RefGetProgress({"loot", "mplootkc", std::to_string(i)})); }

// orig 0x003DA150
struct SHOP
{
    SW* psw = nullptr;
    LO* plo = nullptr;          // +0x24 (0x378)
    int dir = -1;               // +0x28 the stick's direction (vt slot 14: 0x1DC4B0)
    float tDir = 0;             // +0x2C
    int msg = -1;               // +0x30 (0 a question, 1 a message)
    float tMsg = -10.0f;        // +0x34
    int idMsg = -1;             // +0x38
    int page = -1;              // +0x40
    float tPage = 0;            // +0x44
    int c = 0;                  // +0x48
    struct ENT { int id = -1; int idIcon = -1; } aent[0x40];   // +0x4C
    int isel = -1;              // +0x24C
    int first = 0;              // +0x250
    int itemBuy = -1, itemSell = -1;   // +0x254 / +0x258
    int idPrompt = -1, idUp = -1, idDown = -1;   // +0x25C / +0x260 / +0x264 (0x213DB0's handles)
};
SHOP s_shop;

// orig 0x001DA950: the message (0 a question until answered, -1 none, else for 5 seconds)
void SetShopMsg(int n, int id)
{
    s_shop.idMsg = id;
    s_shop.msg = n;
    if (n == 0)
    {
        s_shop.tMsg = FLT_MAX;
        PsndStartSound(0x29);  // (0x1FBB90 + 0x1FB2F8)
    }
    else if (n == -1) s_shop.tMsg = -10.0f;
    else
    {
        s_shop.tMsg = g_clock.tReal + 5.0f;
        PsndStartSound(0x36);
    }
}

int IRowEnd() { return s_shop.first + CROW; }   // orig 0x001DBBA0 (D_003DA570 0)

// orig 0x001DBBE0
void SelectShopRow(int i)
{
    SHOP& p = s_shop;
    if (p.c == 0)
    {
        p.first = 0;
        p.isel = -1;
        p.itemBuy = p.itemSell = -1;
        return;
    }
    i = std::clamp(i, 0, p.c - 1);
    p.isel = i;
    p.first = std::clamp(p.first, std::max(i - CROW + 1, 0), i);
    p.itemBuy = p.itemSell = p.aent[i].id;
}

// orig 0x001DBD10
void BuildShopList()
{
    SHOP& p = s_shop;
    for (int j = 0; j < p.c; ++j) p.aent[j].id = -1;
    p.c = 0;
    if (p.page == 0)
    {
        for (int i = 7; i < 0x2B && p.c < 0x40; ++i)
            if (FItemOpen(i) && !FItemOwned(i)) p.aent[p.c++].id = i;
    }
    else if (p.page == 1)
    {
        for (int i = 0; i < 0x40 && p.c < 0x40; ++i)
            if (CLoot(i) != 0) p.aent[p.c++].id = i;
    }
    SelectShopRow(p.isel);
}

// orig 0x001DBEF0: the page's look (the arrows, the rows, the selection, the prompt)
void UpdateShopLook()
{
    SHOP& p = s_shop;
    if (!p.plo || NScreenObjState(p.plo) != 2) return;
    const int end = IRowEnd();
    SelectScreenObjPartHandle(p.plo, p.first > 0 ? (p.page == 0 ? 0x4DA : 0x4A1) : -1, &p.idUp);
    for (int i = p.first; i < end && i < 0x40; ++i)
    {
        SHOP::ENT& e = p.aent[i];
        const int k = NElf(0x2C3588 + 4 * uint32_t(i - p.first));
        if (p.page == 0)
        {
            if (e.id != -1)
            {
                switch (ItemKind(e.id))
                {
                case 7: e.idIcon = 0x456; break;
                case 9: e.idIcon = 0x458; break;
                case 8: e.idIcon = 0x457; break;
                default: break;
                }
                SelectScreenObjPartAt(p.plo, 0x455, k);
                SelectScreenObjPartAt(p.plo, e.idIcon, k);
            }
            else
            {
                ResetScreenObjPartAt(p.plo, 0x455, k);
                ResetScreenObjPartAt(p.plo, e.idIcon, k);
                e.idIcon = -1;
            }
        }
        else if (p.page == 1)
        {
            if (e.id != -1) SelectScreenObjPartAt(p.plo, 0x455, k);
            else ResetScreenObjPartAt(p.plo, 0x455, k);
        }
    }
    if (p.isel >= 0)
    {
        SelectScreenObjPart(p.plo, 0x437);
        SetScreenObjPartMatrix(p.plo, 0x437, {1, 0, 0, 1, 0, DY_ROW * float(p.isel - p.first)});
    }
    else ResetScreenObjPart(p.plo, 0x437);
    SelectScreenObjPartHandle(p.plo, end < p.c ? (p.page == 0 ? 0x4DD : 0x4A4) : -1, &p.idDown);
    SelectScreenObjPartHandle(p.plo, p.msg != -1 ? 0x43B : 0x439, &p.idPrompt);
}

// orig 0x001DC4B0 (vt slot 14: a direction of the stick)
void SetShopDir(int n)
{
    SHOP& p = s_shop;
    if (n == p.dir) return;
    p.dir = n;
    p.tDir = g_clock.tReal;
    switch (n)
    {
    case 0: SetShopPage(1); break;
    case 4: SetShopPage(0); break;
    case 1: case 2: case 3:
        if (p.isel > 0)
        {
            SelectShopRow(p.isel - 1);
            PsndStartSound(0x35);
        }
        else PsndStartSound(0x36);
        break;
    case 5: case 6: case 7:
        if (p.isel < p.c - 1)
        {
            SelectShopRow(p.isel + 1);
            PsndStartSound(0x35);
        }
        else PsndStartSound(0x36);
        break;
    default: break;
    }
}

// orig 0x001DA858: the stick's direction (eight, from 0.8 out; released under 0.5); the d-pad as the stick
void UpdateShopStick()
{
    float x = g_joy.xLeft, y = g_joy.yLeft;
    const uint32_t grf = GrfbtnHeld();
    if (grf & 0x1000) y = 1;
    if (grf & 0x4000) y = -1;
    if (grf & 0x8000) x = -1;
    if (grf & 0x2000) x = 1;
    const float g = std::sqrt(x * x + y * y);
    if (s_shop.dir == -1)
    {
        if (g > 0.8f) SetShopDir(int(std::atan2(y, x) * 1.2732395f + 8.5f) % 8);
    }
    else if (g < 0.5f) SetShopDir(-1);
}
}  // namespace

// orig 0x0019F0A8: every player (class 0x34, at most 8) gets vt slot 104 (0x141E30): its event 0x1A (vt slot 3 =
// 0x1A61E0: the splice event 0x1A only, the tasks' broadcast being for 5 / 6)
void NotifyPlayersPowerups(SW* psw)
{
    if (!psw) return;
    std::vector<LO*> aplo = AploOfClass(psw, 0x34);
    if (aplo.size() > 8) aplo.resize(8);
    for (LO* plo : aplo) HandleLoSpliceEvent(plo, 0x1A, false, 0, nullptr);
}

void InitShop(SW* psw)
{
    s_shop = SHOP{};
    s_shop.psw = psw;
    s_shop.plo = psw ? PloFindSwObjectByOid(psw, 0x378) : nullptr;
    if (psw && std::getenv("SLY2N_ITEMPRINT"))   // debug: the powerup table (0x2BCD00)
        for (int i = 0; i < 0x2B; ++i)
            std::printf("item %2d kind %d cost %4d power %2d episode %2d assign %2d %s: %s\n", i, ItemKind(i), ItemCost(i),
                        NElf(0x2BCD00 + 0x20 * uint32_t(i) + 0xC), ItemEpisode(i), NElf(0x2BCD00 + 0x20 * uint32_t(i)),
                        ItemName(i) > 0 ? StrLocalized(psw, ItemName(i)).c_str() : "-", ItemDesc(i) > 0 ? StrLocalized(psw, ItemDesc(i)).c_str() : "-");
}

bool FShopShown() { return s_shop.page != -1; }

void SetShopPage(int s)
{
    SHOP& p = s_shop;
    if (!p.plo) s = -1;
    if (s == p.page) return;
    if (p.page == -1)
    {
        SetClockFactor(&g_clock, 0, 0.0f);   // (0x14D1D0: the game paused; vt slot 10: in the HUD's list)
        SetScreenObjState(p.plo, 1);
    }
    const int old = p.page;
    p.page = s;
    p.tPage = g_clock.tReal;
    switch (s)
    {
    case -1:
        if (p.plo)
        {
            HideScreenObj(p.plo);
            if (NScreenObjState(p.plo) > 0) SetScreenObjTimers(p.plo, 1);
        }
        SetClockFactor(&g_clock, 0, 1.0f);
        p.idPrompt = p.idUp = p.idDown = -1;
        break;
    case 0:
    case 1:
        if (s == 0) SetScreenObjParts(p.plo, 0x4D4, 0x4D5, 0x4D6);
        else SetScreenObjParts(p.plo, 0x49B, 0x49C, 0x49D);
        if (old == (s == 0 ? 1 : 0))
        {
            SetScreenObjState(p.plo, 1);
            SetScreenObjState(p.plo, 2);
        }
        else SetScreenObjTimers(p.plo, 2);
        PsndStartSound(0x35);  // (0x1DC260)
        break;
    default: break;
    }
    BuildShopList();
    SelectShopRow(0);
    SetShopDir(-1);
}

// orig 0x001DB870
void UpdateShop(SW* psw)
{
    SHOP& p = s_shop;
    p.psw = psw;
    if (p.tMsg <= g_clock.tReal) SetShopMsg(-1, -1);   // (0x1DA808)
    if (p.page == -1) return;
    if (p.msg != 0 && GrfbtnTakePressed(BtnMenuBack())) { SetShopPage(-1); return; }
    if (p.page == 0)
    {
        if (GrfbtnTakePressed(BtnMenuConfirm()))
        {
            const int item = p.itemBuy;
            if (std::getenv("SLY2N_ITEMPRINT")) std::printf("shop: CROSS on item %d (msg %d, coins %d)\n", item, p.msg, CCoins());
            if (item != -1)
            {
                const int cost = ItemCost(item), have = CCoins();
                if (p.msg == 0)
                {
                    // bought (0x1DAF38: the progress, and the broadcast 0x3B)
                    SetShopMsg(-1, -1);
                    SetCoins(have - cost);
                    CRef r;
                    r.SetBool(true);
                    FSetProgress({"powerUps", "mppukfAcquired", std::to_string(item)}, r);
                    g_taskmgr.nItemC = item;   // (0x1DAF38: the task manager's +0xC)
                    if (psw) BroadcastTaskEvent(psw, 0x3B, nullptr);
                    BuildShopList();
                    PsndStartSound(0x29);  // (0x1FBB90 + 0x1FB2F8)
                    NotifyPlayersPowerups(psw);  // 0x19F0A8
                }
                else if (have < cost) SetShopMsg(1, 0x35);
                else SetShopMsg(0, 0x34);
            }
            else PsndStartSound(0x36);
        }
    }
    else if (p.page == 1)
    {
        if (GrfbtnTakePressed(BtnMenuConfirm()))
        {
            if (p.itemSell == -1) PsndStartSound(0x36);
            else if (const int n = CLoot(p.itemSell); n > 0)
            {
                CRef r;
                r.SetS32(n - 1);
                FSetProgress({"loot", "mplootkc", std::to_string(p.itemSell)}, r);
                SetCoins(CCoins() + LootValue(p.itemSell));
                BuildShopList();
                PsndStartSound(0x29);
            }
        }
    }
    if (p.msg == 0 && GrfbtnTakePressed(BtnMenuBack())) SetShopMsg(-1, -1);
    if (GrfbtnTakePressed(0x80))
    {
        SetShopMsg(-1, -1);
        SetShopPage(p.page == 0 ? 1 : 0);
    }
    if (p.msg != 0) UpdateShopStick();
    UpdateShopLook();
}

std::string StrShopScript(int id)
{
    const SHOP& p = s_shop;
    const SW* psw = p.psw;
    char ach[256];
    if (!p.plo) return "";
    // orig 0x001DC790 / 0x001DC820 (0x43C..0x447): row i's powerup name / cost
    if (id >= 0x43C && id <= 0x441)
    {
        const int n = p.aent[std::min(p.first + id - 0x43C, 0x3F)].id;
        return n >= 0 && n < 0x2B && ItemName(n) != -1 ? StrLocalized(psw, ItemName(n)) : "";
    }
    if (id >= 0x442 && id <= 0x447)
    {
        const int n = p.aent[std::min(p.first + id - 0x442, 0x3F)].id;
        if (n < 0 || n >= 0x2B) return "";
        std::snprintf(ach, sizeof(ach), "%d", ItemCost(n));
        return ach;
    }
    // orig 0x001DC890 / 0x001DC9C0 (0x448..0x453): row i's loot name (with its count) / value
    if (id >= 0x448 && id <= 0x44D)
    {
        const int n = p.aent[std::min(p.first + id - 0x448, 0x3F)].id;
        if (n < 0 || n >= 0x40) return "";
        std::string s = StrLocalized(psw, LootName(n));
        const int c = CLoot(n);
        if (c >= 2) { std::snprintf(ach, sizeof(ach), "%s (%d)", s.c_str(), c); s = ach; }
        return s;
    }
    if (id >= 0x44E && id <= 0x453)
    {
        const int n = p.aent[std::min(p.first + id - 0x44E, 0x3F)].id;
        if (n < 0 || n >= 0x40) return "";
        std::snprintf(ach, sizeof(ach), "%d", LootValue(n));
        return ach;
    }
    if (id == 0x436)
    {
        // orig 0x001DCA30
        switch (p.msg)
        {
        case -1: return StrLocalized(psw, IdText(0xDE));
        case 0: return StrLocalized(psw, IdText(0xDF));
        case 1: return StrLocalized(psw, IdText(0xE0));
        default: return "";
        }
    }
    if (id == 0x435)
    {
        // orig 0x001DCAB0: the message, else the selected item's name, description and whether it is affordable
        if (p.msg != -1) return StrLocalized(psw, IdText(p.idMsg));   // (USA ids: 0x34 / 0x35)
        if (p.page == 0)
        {
            const int i = p.itemBuy;
            if (i >= 0 && i < 0x2B && ItemDesc(i) != p.msg)
            {
                std::snprintf(ach, sizeof(ach), "%s: %s. %s", StrLocalized(psw, ItemName(i)).c_str(), StrLocalized(psw, ItemDesc(i)).c_str(),
                              StrLocalized(psw, IdText(CCoins() < ItemCost(i) ? 0xD9 : 0xDA)).c_str());
                return ach;
            }
            return StrLocalized(psw, IdText(0xDC));
        }
        if (p.page == 1)
        {
            const int i = p.itemSell;
            if (i >= 0 && i < 0x40)
            {
                // (0xDB: "... sell one for %d coins": its value, 0x2C3288 +0)
                const std::string fmt = StrLocalized(psw, IdText(0xDB));
                const size_t ipct = fmt.find("%d");
                std::string str = fmt;
                if (ipct != std::string::npos) str = fmt.substr(0, ipct) + std::to_string(LootValue(i)) + fmt.substr(ipct + 2);
                return str;
                return ach;
            }
            return StrLocalized(psw, IdText(0xE3));
        }
    }
    return "";
}

// ------------------------------------------------------------ the gadget assignment (orig 0x3DA0F0, module
// 0x1DAA08..0x1DB7A8: func_001DB520.c / func_001DB390.c / func_001DB010.c / func_001DAE20.c, the code of 0x1DAA90
// (m_1D93D8_c.cpp) and 0x1DB0C8, func_001DC598.c, func_00141FC8.c)
extern int g_frameJoy;   // (joy.cpp: the step)
namespace
{
struct GADGETENT { int key, idOpen, idOwned, idSel, cur; float x, y; uint32_t grf; };
struct GADGETMENU
{
    SW* psw = nullptr;
    OID oid = -1;        // +0x20 (0x375 Sly / 0x376 Bentley / 0x377 Murray)
    LO* plo = nullptr;   // +0x24
    int dir = -1;        // +0x28
    int msg = -1;        // +0x30
    float tMsg = -10.0f; // +0x34
    int idMsg = -1;      // +0x38
    int ichar = -1;      // +0x40
    int key = -1;        // +0x48 the powerup selected
    std::vector<GADGETENT> aent;   // +0x4C / +0x50 (0x2C2DE8 / 0x2C3028 / 0x2C3148)
};
GADGETMENU s_gm;
const char* const s_aszChar[3] = {"sly", "bentley", "murray"};

void SetGmMsg(int n, int id)
{
    s_gm.idMsg = id;
    s_gm.msg = n;
    s_gm.tMsg = n == 0 ? FLT_MAX : n == -1 ? -10.0f : g_clock.tReal + 5.0f;
    if (n != -1) PsndStartSound(n == 0 ? 0x29 : 0x36);  // (0x1DA950: 0x1FBB90 + 0x1FB2F8)
}
GADGETENT* PgeFind(int key)
{
    for (GADGETENT& e : s_gm.aent)
        if (e.key == key) return &e;
    return nullptr;
}
int IslotAssigned(int key)
{
    if (s_gm.ichar < 0) return -1;
    for (int j = 0; j < 3; ++j)
        if (NRef(RefGetProgress({s_aszChar[s_gm.ichar], "apukCur", std::to_string(j)})) == key) return j;
    return -1;
}

// orig 0x001DB390
void SelectGadget(int key)
{
    GADGETMENU& p = s_gm;
    p.key = key;
    for (GADGETENT& e : p.aent)
    {
        const int v = e.key == key ? e.idSel : (e.grf & 4) ? e.idOwned : (e.grf & 2) ? e.idOpen : -1;
        if (v == e.cur) continue;
        if (p.plo && e.cur != -1) ResetScreenObjPart(p.plo, e.cur);
        e.cur = v;
        if (p.plo && v != -1) SelectScreenObjPart(p.plo, v);
    }
}

// orig 0x001DB010: the entries open (bit 2) and owned (bit 4)
void UpdateGadgetFlags()
{
    for (GADGETENT& e : s_gm.aent)
    {
        e.grf = (e.grf & ~6u) | (FItemOpen(e.key) ? 2u : 0u) | (FItemOwned(e.key) ? 4u : 0u);
    }
}

// orig 0x001DB0C8 (vt slot 14): the stick's direction picks the nearest owned gadget along it
void StepGadget(int dir, float x, float y)
{
    GADGETMENU& p = s_gm;
    if (dir == p.dir) return;
    p.dir = dir;
    if (dir == -1 || p.key == -1) return;
    const GADGETENT* pcur = PgeFind(p.key);
    if (!pcur) return;
    const glm::vec2 p0(pcur->x, pcur->y);
    glm::vec2 d(x, -y);
    if (glm::length(d) < 1e-6f) return;
    d = glm::normalize(d);
    float best = FLT_MAX;
    int keyBest = -1;
    for (const GADGETENT& e : p.aent)
    {
        if (&e == pcur || !(e.grf & 4)) continue;
        const glm::vec2 c(e.x, e.y), w = c - p0;
        const float t = glm::dot(w, d);
        const float dist = glm::length(w - d * t);
        float score = FLT_MAX;
        if (t > 0.0f && t <= 750.0f && dist <= 40.0f) score = (t - std::sqrt(std::max(0.0f, 1600.0f - dist * dist)));   // (0x14C748: the hit along the ray)
        else if (t > 0.0f && dist < 200.0f) score = t + dist;                                                      // (0x11EE08: t x 750 + distance)
        if (score < best) { best = score; keyBest = e.key; }
    }
    if (keyBest != -1)
    {
        SelectGadget(keyBest);
        PsndStartSound(0x35);  // (0x1DB0C8)
    }
}
}  // namespace

// orig 0x001DB520
void SetGadgetMenu(int s)
{
    GADGETMENU& p = s_gm;
    if (s == p.ichar) return;
    if (p.ichar == -1) SetClockFactor(&g_clock, 0, 0.0f);
    SelectGadget(-1);
    p.ichar = s;
    switch (s)
    {
    case -1:
        if (p.plo) HideScreenObj(p.plo);
        for (GADGETENT& e : p.aent) e.cur = -1;
        p.aent.clear();
        SetClockFactor(&g_clock, 0, 1.0f);
        SetGmMsg(-1, -1);
        p.plo = nullptr;
        p.oid = -1;
        return;
    case 0: p.oid = 0x375; break;
    case 1: p.oid = 0x376; break;
    case 2: p.oid = 0x377; break;
    default: return;
    }
    static const uint32_t s_ava[3] = {0x2C2DE8, 0x2C3028, 0x2C3148};
    static const int s_ac[3] = {0x12, 9, 8};
    p.aent.clear();
    for (int i = 0; i < s_ac[s]; ++i)
    {
        const uint32_t v = s_ava[s] + 0x20 * uint32_t(i);
        GADGETENT e;
        e.key = NElf(v); e.idOpen = NElf(v + 4); e.idOwned = NElf(v + 8); e.idSel = NElf(v + 0xC); e.cur = -1;
        int32_t n;
        n = NElf(v + 0x14); std::memcpy(&e.x, &n, 4);
        n = NElf(v + 0x18); std::memcpy(&e.y, &n, 4);
        e.grf = uint32_t(NElf(v + 0x1C));
        p.aent.push_back(e);
    }
    UpdateGadgetFlags();
    if (std::getenv("SLY2N_GADGETPRINT"))
        for (const GADGETENT& e : p.aent) std::printf("gadget: key %d at %.1f %.1f grf %u\n", e.key, e.x, e.y, e.grf);
    p.plo = p.psw ? PloFindSwObjectByOid(p.psw, p.oid) : nullptr;
    if (!p.plo) { SetGadgetMenu(-1); return; }
    ShowScreenObj(p.plo, 0x318);
    // the owned one nearest the top left
    float best = FLT_MAX;
    int key = -1;
    for (const GADGETENT& e : p.aent)
        if ((e.grf & 4) && e.x + e.y < best) { best = e.x + e.y; key = e.key; }
    SelectGadget(key);
}

bool FGadgetMenuShown() { return s_gm.ichar != -1; }

// orig 0x001DB490 (the player's Select): the menu of the current character
void OpenGadgetMenu(SW* psw, LO* ploPlayer)
{
    s_gm.psw = psw;
    if (!ploPlayer) return;
    const CID cid = ploPlayer->Cid();
    if (FCidDerivedFrom(cid, 0x35)) SetGadgetMenu(0);
    else if (FCidDerivedFrom(cid, 0x37)) SetGadgetMenu(1);
    else if (FCidDerivedFrom(cid, 0x36)) SetGadgetMenu(2);
}

// orig 0x001DAA90
void UpdateGadgetMenu(SW* psw, LO* ploPlayer)
{
    GADGETMENU& p = s_gm;
    p.psw = psw;
    if (p.tMsg <= g_clock.tReal) SetGmMsg(-1, -1);
    if (!p.plo || p.ichar == -1) return;
    if (p.msg != 0 && GrfbtnTakePressed(BtnMenuBack())) { SetGadgetMenu(-1); return; }
    if (!ploPlayer || !FCidDerivedFrom(ploPlayer->Cid(), 0x34)) { SetGadgetMenu(-1); return; }
    UpdateGadgetFlags();
    // orig 0x001DA858 with the d-pad as the stick
    {
        float x = g_joy.xLeft, y = g_joy.yLeft;
        const uint32_t grf = GrfbtnHeld();
        if (grf & 0x1000) y = 1;
        if (grf & 0x4000) y = -1;
        if (grf & 0x8000) x = -1;
        if (grf & 0x2000) x = 1;
        const float g = std::sqrt(x * x + y * y);
        if (p.dir == -1) { if (g > 0.8f) StepGadget(int(std::atan2(y, x) * 1.2732395f + 8.5f) % 8, x, y); }
        else if (g < 0.5f) StepGadget(-1, x, y);
    }
    // (PC menus) the mouse: an owned gadget under the cursor is selected when the cursor moves onto it or it is
    // clicked (a click on the selected one confirms, as Cross) (its cell: 80 x 80 grid units around (x + 26.5, y - 8), the grid's y in the HUD's 1.1 scale); the wheel
    // steps as the d-pad (pcinput.cpp)
    if (float xc = 0, yc = 0; FMenuCursor(&xc, &yc))
        for (const GADGETENT& e : p.aent)
        {
            if (!(e.grf & 4) || std::fabs(xc - (e.x + 26.5f)) >= 40.0f || std::fabs(yc - (e.y - 8.0f) / 1.1f) >= 40.0f / 1.1f) continue;
            if (e.key == p.key) break;   // (a click on the selected one: the confirm button, as before)
            SetMenuMouseTarget();
            if (FMenuCursorMoved() || FMenuClick())
            {
                SelectGadget(e.key);
                PsndStartSound(0x35);  // (as a step: 0x1DB0C8)
            }
            break;
        }
    if (GrfbtnTakePressed(0x7E8))
    {
        const GADGETENT* r = PgeFind(p.key);
        SetGmMsg(1, r && (r->grf & 1) ? 0x31 : 0xE2);
    }
    static const uint32_t s_agrfSlot[3] = {4, 1, 2};   // (0x2C3258: L1, L2, R2)
    for (int i = 0; i < 3; ++i)
    {
        if (!GrfbtnTakePressed(s_agrfSlot[i])) continue;
        const GADGETENT* r = PgeFind(p.key);
        if (!r || !(r->grf & 4)) SetGmMsg(1, 0x32);
        else if (!(r->grf & 1)) SetGmMsg(1, 0x33);
        else
        {
            // orig 0x00141FC8 on the player (gadget.cpp): slot i takes it (another slot holding it emptied), the
            // progress "<character> apukCur"
            if (ACTOR* pa = ploPlayer ? PactorFromLo(ploPlayer) : nullptr)
            {
                SetActorAbilitySlot(pa, i, p.key);
                PsndStartSound(0x34);  // (0x1DAA90)
                continue;
            }
            for (int j = 0; j < 3; ++j)
            {
                const std::vector<std::string> apath{s_aszChar[p.ichar], "apukCur", std::to_string(j)};
                CRef ref;
                if (j == i) ref.SetS32(p.key);
                else if (NRef(RefGetProgress(apath)) == p.key) ref.SetS32(-1);
                else continue;
                FSetProgress(apath, ref);
            }
            PsndStartSound(0x34);  // (0x1DAA90)
        }
    }
}

std::string StrGadgetScript(int id)
{
    // orig 0x001DC598 (0x37A / 0x37B): the message, else the selected gadget's name, description and its button
    if ((id != 0x37A && id != 0x37B) || s_gm.ichar == -1) return "";
    const SW* psw = s_gm.psw;
    if (s_gm.msg != -1) return StrLocalized(psw, IdText(s_gm.idMsg));   // (USA ids: 0x31..0x33, 0xE2)
    if (s_gm.key == -1) return StrLocalized(psw, IdText(0xDD));
    const GADGETENT* po = PgeFind(s_gm.key);
    const int v = IslotAssigned(s_gm.key);
    static const char* const s_aszIcon[3] = {"&2L&.", "&2l&.", "&2r&."};
    std::string buf;
    std::string desc;
    if (po && (po->grf & 1) && v < 0) buf = StrLocalized(psw, IdText(0xE1));
    else
    {
        const std::string fmt = StrLocalized(psw, NElf(0x2BCD00 + 0x20 * uint32_t(s_gm.key) + 0x1C));
        const char* sz = v >= 0 ? s_aszIcon[v] : "";
        for (size_t i = 0; i < fmt.size(); ++i)
        {
            if (fmt[i] == '%' && i + 1 < fmt.size() && fmt[i + 1] == 's') { buf += sz; ++i; }
            else buf += fmt[i];
        }
    }
    desc = StrLocalized(psw, ItemDesc(s_gm.key));
    return StrLocalized(psw, ItemName(s_gm.key)) + (desc.empty() ? "" : ": ") + desc + ". " + buf;
}

// orig 0x001DAC58: on each entry assigned to the player's slot j, that slot's button ("&2L&." L1, "&2l&." L2,
// "&2r&." R2: D_002C3268) in the icon font at scale 0.66 (D_002C3264), a 32 x 32 box at the entry's +0x14 / +0x18,
// colour 0x80808080, left / top (0x2163B0 at priority 0x13)
void DrawGadgetIcons(const FONTBRX* pfont, const FONTBRX* picons)
{
    if (!pfont || s_gm.ichar < 0 || !s_gm.plo || !FScreenObjVisible(s_gm.plo)) return;
    static const char* const s_asz[3] = {"&2L&.", "&2l&.", "&2r&."};
    const float size = 0.66f * 51.2f;
    for (const GADGETENT& e : s_gm.aent)
    {
        const int j = IslotAssigned(e.key);
        if (j < 0) continue;
        g_hud.DrawTextBox(*pfont, picons, s_asz[j], e.x - 320.0f, e.y - 246.4f, size, 1.0f, 1.0f, 32.0f, 32.0f, 0, 0,
                          glm::vec4(128.0f, 128.0f, 128.0f, 0.0f), glm::vec4(1.0f), glm::vec4(128.0f, 128.0f, 128.0f, 0.0f),
                          g_zScreenBase);   // (over the screen objects: DrawScreenObjs sets it above their priorities)
    }
}

// ------------------------------------------------------------ the job help (orig 0x3DA3C0 on 0x4E0: func_001DD300.c /
// func_001DCED8.cpp / func_001DCE90.c / func_001DD1F8.c / func_001DD558.c / func_001DDDB8..func_001DDF00.c /
// func_00163400.c / func_00164030.c / func_00163430.c / func_00165C28.c, m_1DAC58_p3.cpp's 0x1DCD30)
std::vector<TASK*> AptaskInOrder(SW* psw);
namespace
{
struct JOBHELP
{
    SW* psw = nullptr;
    LO* plo = nullptr;          // +0x24 (0x4E0)
    int dir = -1;               // +0x28
    int page = -1;              // +0x40 (0 the jobs, 1 the job's goals)
    float tPage = 0;
    struct ENT { LO* plo = nullptr; int idA = -1, idB = -1; };
    std::vector<ENT> aent;      // +0x48 / +0x4C
    int cMax = 10;              // +0x10C
    JOB* pjob = nullptr;        // +0x110 the job shown on page 1
    bool fFromJobs = false;     // +0x114
    int isel = 0;               // +0x118
    int idA = -1, idB = -1, idC = -1;
};
JOBHELP s_jh;
// (PC menus) the rows' text as last drawn (NoteJobHelpText: HUD units, x from the canvas centre, y x 1.1 from it)
struct JHROW { float x = 0, y = 0, size = 0; int frame = -100; };
JHROW s_ajhrow[10];

// orig 0x00164030: a task done (or its progress "closed" value while it has no state)
bool FTaskDoneJh(const TASK* p)
{
    if (p->nState == -1 && p->prD8.F()) return NRef(RefGetProgressKey(p->prD8.akey)) != 0;
    return p->nState >= 2;
}
// orig 0x00163430: a job open (its first task's state, else its last's, at least 1)
int NJobOpen(const JOB* p)
{
    if (!p->ptaskFirst) return 0;
    const int n = p->ptaskFirst->nState;
    return n < 2 ? n : std::max(p->ptaskLast ? p->ptaskLast->nState : 1, 1);
}

// orig 0x001DCED8: the page's rows
void BuildJobHelp()
{
    JOBHELP& p = s_jh;
    for (JOBHELP::ENT& e : p.aent)
    {
        if (e.idA != -1) ResetScreenObjPart(p.plo, e.idA);
        if (e.idB != -1) ResetScreenObjPart(p.plo, e.idB);
    }
    p.aent.clear();
    p.isel = 0;
    std::vector<TASK*> a = AptaskInOrder(p.psw);
    std::stable_sort(a.begin(), a.end(), [](const TASK* x, const TASK* y) {
        if (x->nJobDepth != y->nJobDepth) return x->nJobDepth < y->nJobDepth;
        const int n1 = x->pjob ? x->pjob->oid : -1, n2 = y->pjob ? y->pjob->oid : -1;
        if (n1 != n2) return n1 < n2;
        if (x->nDepth != y->nDepth) return x->nDepth < y->nDepth;
        return x->oid < y->oid;
    });
    if (p.page == 0)
    {
        JOB* plast = nullptr;
        for (TASK* pt : a)
        {
            JOB* pj = pt->pjob;
            if (!pj || pj == plast) continue;
            plast = pj;
            if (NJobOpen(pj) > 0 && pj->n6c != -1)
            {
                if (p.pjob == pj) p.isel = int(p.aent.size());
                p.aent.push_back({pj});
            }
        }
    }
    else if (p.page == 1)
    {
        bool f = false;
        for (TASK* pt : a)
        {
            if (pt->pjob != p.pjob || pt->idDialog5C == -1) continue;
            if (f && pt->nState == 0) break;
            p.aent.push_back({pt});
            if (pt->nState == 1) f = true;
            if (pt->nState == 0) break;
        }
    }
    if (int(p.aent.size()) > p.cMax)
    {
        const int d = int(p.aent.size()) - p.cMax;
        p.aent.erase(p.aent.begin(), p.aent.begin() + d);
        p.isel = d < p.isel ? p.isel - d : 0;
    }
    p.isel = p.aent.empty() ? -1 : std::clamp(p.isel, 0, int(p.aent.size()) - 1);
}

// orig 0x001DD1F8
void UpdateJobHelpLook()
{
    JOBHELP& p = s_jh;
    if (!p.plo || NScreenObjState(p.plo) != 2) return;
    for (size_t i = 0; i < p.aent.size(); ++i)
    {
        JOBHELP::ENT& e = p.aent[i];
        SelectScreenObjPartHandle(p.plo, int(i) == p.isel ? int(i) + 0x4FD : int(i) + 0x4E9, &e.idA);
        bool f = false;
        if (p.page == 0)
        {
            const JOB* pj = static_cast<const JOB*>(e.plo);
            f = pj->ptaskLast && FTaskDoneJh(pj->ptaskLast);
        }
        else f = FTaskDoneJh(static_cast<const TASK*>(e.plo));
        SelectScreenObjPartHandle(p.plo, f ? int(i) + 0x507 : -1, &e.idB);
    }
}
}  // namespace

// orig 0x001DD300
void SetJobHelpPage(int s)
{
    JOBHELP& p = s_jh;
    if (!p.plo) s = -1;
    if (s == p.page) return;
    const int old = p.page;
    if (old == -1) PauseGame(0);  // orig: 0x001DD300, acquire the gameplay-input pause on entry.
    p.page = s;
    p.tPage = g_clock.tReal;
    switch (s)
    {
    case -1:
        ResumeGame(0);  // orig: 0x001DD300, release only when the help closes.
        if (p.plo && NScreenObjState(p.plo) > 0) SetScreenObjTimers(p.plo, 1);
        if (p.plo) HideScreenObj(p.plo);
        p.idA = p.idB = p.idC = -1;
        break;
    case 0:
        p.cMax = 10;
        SetScreenObjParts(p.plo, old == 1 ? 0x4E8 : 0x4E1, 0x4E3, 0x4E2);
        SetScreenObjState(p.plo, 1);
        SetScreenObjTimers(p.plo, 2);
        break;
    case 1:
        if (p.isel >= 0 && p.isel < int(p.aent.size())) p.pjob = static_cast<JOB*>(p.aent[size_t(p.isel)].plo);
        else p.pjob = p.psw ? p.psw->tasks.pjobCur : nullptr;
        p.fFromJobs = old == 0;
        p.cMax = 4;
        SetScreenObjParts(p.plo, old == 0 ? 0x4E7 : 0x4E4, 0x4E6, 0x4E5);
        SetScreenObjState(p.plo, 1);
        SetScreenObjTimers(p.plo, 2);
        break;
    default: break;
    }
    BuildJobHelp();
    p.dir = -1;
}

void InitJobHelp(SW* psw)
{
    s_jh = JOBHELP{};
    s_jh.psw = psw;
    s_jh.plo = psw ? PloFindSwObjectByOid(psw, 0x4E0) : nullptr;
    // orig: 0x001DCCA0: Job Help's screen pauses the world while its panel is shown.
    if (s_jh.plo)
    {
        s_jh.plo->SetField(0, 0xB0, 1);
        SetScreenObjPauses(s_jh.plo, true);
    }
}
bool FJobHelpShown() { return s_jh.page != -1; }

// orig 0x001DCD30
void UpdateJobHelp(SW* psw)
{
    JOBHELP& p = s_jh;
    p.psw = psw;
    if (p.page == -1 || !p.plo) return;
    if (GrfbtnTakePressed(0x800)) { SetJobHelpPage(-1); return; }
    if (p.page == 0)
    {
        if (GrfbtnTakePressed(BtnMenuBack())) { SetJobHelpPage(-1); return; }
        if (!p.aent.empty() && GrfbtnTakePressed(0xE0)) { SetJobHelpPage(1); return; }
    }
    else if (p.page == 1)
    {
        if (!p.fFromJobs && GrfbtnTakePressed(BtnMenuBack())) { SetJobHelpPage(-1); return; }
        if (GrfbtnTakePressed(0xF0)) { SetJobHelpPage(0); return; }
    }
    // (PC menus) the mouse: on the jobs page the row under the cursor is selected when the cursor moves onto it,
    // and a click opens it (as Cross / right); on a job's page a click on "< more jobs" goes back to the jobs (as
    // left). The wheel steps as the d-pad, the right button goes back (pcinput.cpp).
    if (float xc = 0, yc = 0; FMenuCursor(&xc, &yc) && NScreenObjState(p.plo) == 2)
    {
        if (p.page == 0)
        {
            for (int i = 0; i < int(p.aent.size()) && i < 10; ++i)
            {
                const JHROW& r = s_ajhrow[i];
                if (g_frameJoy - r.frame > 8) continue;   // (not drawn lately)
                // (the row's band, as the selected row's bar: from 6 units above its text's top, the rows 35 / 1.1
                // canvas units apart; from the check mark to the panel's end)
                const float yTop = r.y / 1.1f + 224.0f - 6.0f, xLeft = r.x + 320.0f - 45.0f;
                if (xc < xLeft || xc >= 600.0f || yc < yTop || yc >= yTop + 35.0f / 1.1f) continue;
                SetMenuMouseTarget();
                if ((FMenuCursorMoved() || FMenuClick()) && i != p.isel)
                {
                    p.isel = i;
                    PsndStartSound(0x35);
                }
                if (FMenuClick()) { SetJobHelpPage(1); return; }
                break;
            }
        }
        else if (p.page == 1 && xc >= 60.0f && xc < 230.0f && yc >= 400.0f && yc < 440.0f)
        {
            SetMenuMouseTarget();
            if (FMenuClick()) { SetJobHelpPage(0); return; }
        }
    }
    // orig 0x001DA858 / 0x001DD558: the stick (the d-pad as it)
    float x = g_joy.xLeft, y = g_joy.yLeft;
    const uint32_t grf = GrfbtnHeld();
    if (grf & 0x1000) y = 1;
    if (grf & 0x4000) y = -1;
    if (grf & 0x8000) x = -1;
    if (grf & 0x2000) x = 1;
    const float g = std::sqrt(x * x + y * y);
    int n = p.dir;
    if (p.dir == -1) { if (g > 0.8f) n = int(std::atan2(y, x) * 1.2732395f + 8.5f) % 8; }
    else if (g < 0.5f) n = -1;
    if (n != p.dir)
    {
        p.dir = n;
        switch (n)
        {
        case 0: if (!p.aent.empty()) SetJobHelpPage(1); break;
        case 4: SetJobHelpPage(0); break;
        case 1: case 2: case 3: if (p.isel > 0) { --p.isel; PsndStartSound(0x35); } break;  // (0x1DD558)
        case 5: case 6: case 7: if (p.isel < int(p.aent.size()) - 1) { ++p.isel; PsndStartSound(0x35); } break;
        default: break;
        }
    }
    UpdateJobHelpLook();
}

void NoteJobHelpText(int kind, int id, float x, float y, float size)
{
    if (s_jh.page == -1 || kind != 1 || id < 0x51E || id > 0x527) return;
    JHROW& r = s_ajhrow[id - 0x51E];
    if (std::getenv("SLY2N_JHPRINT") && r.frame != g_frameJoy) std::printf("jobhelp: row %d at %.1f %.1f size %.1f\n", id - 0x51E, x, y, size);
    r = JHROW{x, y, size, g_frameJoy};
}

std::string StrJobHelpScript(int id)
{
    const JOBHELP& p = s_jh;
    const SW* psw = p.psw;
    if (p.page == -1) return "";
    if (id == 0x51B) return StrLocalized(psw, IdText(0x6D + std::clamp(IepisodeCur(), 0, 8)));   // orig 0x001DDDB8
    if (id == 0x51C) return p.pjob ? StrLocalized(psw, p.pjob->n6c) : "";                 // orig 0x001DDE58
    if (id == 0x51D) return p.pjob ? StrLocalized(psw, p.pjob->n70) : "";                 // orig 0x001DDE10
    if (id >= 0x51E && id <= 0x527)
    {
        // orig 0x001DDEA0: row i (a job's +0x6C)
        const size_t i = size_t(id - 0x51E);
        if (i >= p.aent.size()) return "";
        if (p.page == 0) return StrLocalized(psw, static_cast<const JOB*>(p.aent[i].plo)->n6c);
        return StrLocalized(psw, static_cast<const TASK*>(p.aent[i].plo)->f6c);
    }
    if (id >= 0x528 && id <= 0x52B)
    {
        // orig 0x001DDF00: row i (a task's +0x5C)
        const size_t i = size_t(id - 0x528);
        if (i >= p.aent.size() || p.page != 1) return "";
        return StrLocalized(psw, static_cast<const TASK*>(p.aent[i].plo)->idDialog5C);
    }
    return "";
}

// (debug harness, autoplay.cpp: the buy page's selected item (-1: another page / none) and its message (-1 none,
// 0 the confirmation, 1 too few coins))
int ShopItemSelected(int* pmsg)
{
    if (pmsg) *pmsg = s_shop.msg;
    return s_shop.page == 0 ? s_shop.itemBuy : -1;
}
