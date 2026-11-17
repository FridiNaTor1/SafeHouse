// See screen3.h.

#include "screen3.h"
#include "pcloader.h"

#include "menu.h"
#include "epmenu3.h"
#include "jobend3.h"
#include "gauge3.h"
#include "game/task3.h"
#include "lo.h"
#include "clock.h"
#include "screenrt.h"
#include "sound.h"
#include "splayer.h"
#include "actor.h"
#include "challenge3.h"
#include "cls.h"
#include "cplcy.h"
#include "disc.h"
#include "photo.h"
#include "po.h"
#include "region.h"
#include "shop.h"
#include "savedata.h"
#include "sw.h"
#include "game/sly3.h"

#include <algorithm>
#include <map>

namespace
{
const LO* s_ploDrawing3 = nullptr;   // (the screen object whose text is drawn: SetScreenDrawing3)
int s_idPrompt = -1;          // orig3 0x46F788
int s_idCardMessage = -1;     // the card 0x3751D0 +0x50
int s_kindCardPrompt = 0;     // NCardPrompt(+0x50)
bool s_fTitleProgress = false;
std::map<int, std::function<std::string()>>& Mpidfn()
{
    static std::map<int, std::function<std::string()>> s_map;
    return s_map;
}

// The ids of orig3 0x47A4B8 (178 entries; the ranges below hold every one: a reference RAM dump of the title)
bool FSly3ScriptString(int id)
{
    return (id >= 812 && id <= 853) || (id >= 864 && id <= 866) || (id >= 917 && id <= 929) || (id >= 998 && id <= 1035) ||
           (id >= 1110 && id <= 1115) || (id >= 1218 && id <= 1248) || (id >= 1449 && id <= 1500) ||
           (id >= 1692 && id <= 1704) || (id >= 1960 && id <= 1966);
}
}  // namespace

void SetScreenDrawing3(const LO* plo) { s_ploDrawing3 = plo; }
const LO* PloScreenDrawing3() { return s_ploDrawing3; }
void SetPromptText3(int idText) { s_idPrompt = idText; }
int IdPromptText3() { return s_idPrompt; }
void SetCardMessage3(int idText, int kindPrompt)
{
    s_idCardMessage = idText;
    s_kindCardPrompt = kindPrompt;
}
void SetTitleHasProgress3(bool f) { s_fTitleProgress = f; }
void SetScriptString3(int id, std::function<std::string()> fn)
{
    if (fn) Mpidfn()[id] = std::move(fn);
    else Mpidfn().erase(id);
}

std::string StrText3(const SW* psw, int id)
{
    if (id < 0) return std::string();
    std::string str = StrLocalized(psw, id);
    // (PC, copsmap3.h: a screen object copied from a kept world (Cops and Robbers' score board on another hub) shows its
    // own world's texts, e.g. "Sly Wins!")
    if (str.empty() && s_ploDrawing3)
        if (const int iwrec = IwrecOfPcCopy(s_ploDrawing3); iwrec >= 0)
            if (const SW* pswKept = PswKeptWorld(iwrec)) str = StrLocalized(pswKept, id);
    return str;
}

int IlootIndexOfPickup(LO* plo);   // pickup.cpp
int CClueCollectedCur();           // vault.cpp
int CClueNeededCur();
namespace
{
// orig3: 0x0025F1D8: player 0's character (class 0x33) in the binocucom: its zoom (+0xF78 mode 0: the recon policy's
// +0x3C, 1: the free policy's +0xC4, times +0xF94; natively the copied binocucom's g3C) as "x%d" (Sly, 0x34) or
// "x%5.3f" (Bentley, 0x36; D_00327EE8 / D_00327EF0); Murray (Sly 2's Far / Near / YIKES) has none in Sly 3
std::string StrBinocZoom3(const SW* psw)
{
    LO* plo = psw ? PloPoCurrent(const_cast<SW*>(psw)) : nullptr;
    ACTOR* pa = plo && FCidDerivedFrom(plo->Cid(), S3CID(0x34)) ? PactorFromLo(plo) : nullptr;
    if (!pa) return {};
    float g = 0.0f;
    switch (pa->nF20)
    {
    case 0: g = g_cmpolicies.cprecon.sZoom * pa->binoc.g3C; break;
    case 1: g = g_cmpolicies.cpfree.sZoom * pa->binoc.g3C; break;
    default: break;
    }
    char ach[32];
    if (FCidDerivedFrom(plo->Cid(), S3CID(0x35))) std::snprintf(ach, sizeof(ach), "x%d", int(g));
    else if (FCidDerivedFrom(plo->Cid(), S3CID(0x37))) std::snprintf(ach, sizeof(ach), "x%5.3f", double(g));
    else return {};
    return ach;
}

// orig3: 0x0025F628 / 0x0025F720: player 0's character in the interaction of kind 12 (+0x508, the pick / pickpocket
// pose) with its object (+0x9E0): a LOOT (0x6A) by its kind's row of 0x349070 ({amount, text, -} x 12 bytes), a PRIZE
// (0x68) by its own text (+0x19C; natively Sly 2's +0x224) and amount (+0x1A4), else the actor's text (+0x1404;
// natively +0x1318) with no amount; the amount as "%d" (D_00327ED0), none when 0 for a prize
std::string StrLootCard3(const SW* psw, bool fAmount)
{
    LO* plo = psw ? PloPoCurrent(const_cast<SW*>(psw)) : nullptr;
    ACTOR* pa = plo && FCidDerivedFrom(plo->Cid(), S3CID(0x34)) ? PactorFromLo(plo) : nullptr;
    if (!pa || pa->n4C8 != 0xC || !pa->inter.ploTarget) return {};
    LO* py = pa->inter.ploTarget;
    int idText = -1, n = 0;
    bool fN = false;
    if (FCidDerivedFrom(py->Cid(), S3CID(0x5E)))
    {
        const int iloot = IlootIndexOfPickup(py);
        if (iloot >= 0)
        {
            n = int(g_disc.U32Elf(S3VA(0x2C3288) + 0xC * uint32_t(iloot)));
            idText = int(g_disc.U32Elf(S3VA(0x2C3288) + 0xC * uint32_t(iloot) + 4));
            fN = true;
        }
    }
    else if (FCidDerivedFrom(py->Cid(), S3CID(0x5C)))
    {
        idText = int(py->U32Field3(0, 0x224, uint32_t(-1)));
        n = int(py->U32Field3(0, 0x1A4, 0));
        fN = n != 0;
    }
    else idText = pa->n1318;
    if (!fAmount) return StrText3(psw, idText);
    return fN ? std::to_string(n) : std::string();
}
}  // namespace

std::string StrScreenScript3(const SW* psw, int id, const int* aarg, bool* pf)
{
    *pf = FSly3ScriptString(id);
    if (!*pf) return {};
    // 864..866 (0x25F540): the drawn object's own text by its vt slot 55: a task counter's (0x9F, 0x260558: its done /
    // count / left), class 0xA0's (0x260640: +0x120 / +0x124 / their difference, EOPIDs 1244 / 1245, written by its
    // update 0x2605B8 while 0x37B plays), else (the two-player score board 0xA4) the supplied ones; none: "Unknown"
    if (id >= 864 && id <= 866 && s_ploDrawing3)
    {
        std::string str;
        if (s_ploDrawing3->Cid() == S3CID(0x81) && FCounterText3(s_ploDrawing3, id - 864, &str)) return str;
        if (s_ploDrawing3->Cid() == 0xA0)
        {
            auto opt = [&](int eopid) {
                for (const OPT& o : s_ploDrawing3->aopt)
                    if (o.eopid == eopid && !o.aval.empty()) return o.aval[0].n;
                return 0;
            };
            const int a = opt(1244), b = opt(1245);
            return std::to_string(id == 864 ? a : id == 865 ? b : b - a);
        }
    }
    if (auto it = Mpidfn().find(id); it != Mpidfn().end()) return it->second();
    using namespace sly3ui;
    // orig3: 0x0025F4F8 (834..843): the text of argument i (+0xBC + 4 i)
    if (id >= STR_ARG0 && id < STR_ARG0 + 10) return aarg ? StrText3(psw, aarg[id - STR_ARG0]) : std::string();
    switch (id)
    {
    case STR_SUBTITLE:   // 0x25F598
    {
        std::string str;
        uint32_t rgba = 0;
        return FDialogSubtitle(&str, &rgba) ? str : std::string();
    }
    case STR_SUBTITLE_BUTTONS:   // 0x25F5C0 (0x27EEC8: Sly 2's 0x227940, the line's buttons as "&2<glyph>&.")
    {
        static const struct { uint32_t mask; char ch; } s_a[16] = {
            {0x40, 'X'}, {0x20, 'O'}, {0x10, 'T'}, {0x80, 'S'}, {0x800, '!'}, {0x100, '_'}, {0x1000, '^'}, {0x4000, 'v'},
            {0x8000, '<'}, {0x2000, '>'}, {0x4, 'L'}, {0x1, 'l'}, {0x200, '('}, {0x8, 'R'}, {0x2, 'r'}, {0x400, ')'}};
        std::string str;
        for (const auto& e : s_a)
            if (g_dialogsub.grf & e.mask) str += std::string("&2") + e.ch + "&.";
        return str;
    }
    case 812:   // 0x25F0E8: "%d" the coins (a coin counter, class 0xA7: the count it shows)
        return std::to_string(CCoinsShown3(s_ploDrawing3));
    case 814:   // 0x25F158: "%d:%02d" the HUD timer's time left (0x47A3E0 - the clock)
    {
        const int n = int(std::max(0.0f, THudTimerEnd() - g_clock.t));
        char ach[32];
        std::snprintf(ach, sizeof(ach), "%d:%02d", n / 60, n % 60);
        return ach;
    }
    case 813:   // 0x25F098: "%d/%d" the world's counted objects (SW +0x4F58 / +0x4F5C; Sly 2's clue bottles 0x2D0)
    {
        char ach[32];
        std::snprintf(ach, sizeof(ach), "%d/%d", CClueCollectedCur(), CClueNeededCur());
        return ach;
    }
    case 825:   // 0x25F1D8: the binocucom's zoom readout (player 0's character: Sly "x%d", Bentley "x%5.3f", else none)
        return StrBinocZoom3(psw);
    case 826: case 827: case 828: case 829: case 830: case 831: case 832: case 833:
        // 0x25F2E8 (826 the subject chosen: 0x24C6A0 = +0xBC, 827 the feedback: 0x24C668 = +0xC0) and 0x25F398 (828..833:
        // the listed targets' titles) of player 0's photo prompt (+0xF90): Sly 2's 0x2DD..0x2E4 (photo.cpp)
        return StrPhotoScript(0x2DD + (id - 826));
    case 849: case 850:   // 0x25F628 / 0x25F720: the loot being picked / pickpocketed: its name, its amount (card 7331)
        return StrLootCard3(psw, id == 850);
    case 1702:   // 0x222750: "%d%%" the game's percent (0x25E6D0 of the challenge's saved record while one runs, else the game)
    {
        char ach[16];
        const GAMESAVE* pgs = PgsChallengeSaved3();
        std::snprintf(ach, sizeof(ach), "%d%%", NGamePercent3(pgs ? *pgs : g_gamesave));
        return ach;
    }
    case 1704:   // 0x2226F0: text 0x4F "Back" + " : " (D_003261B8) + the back glyph (D_003261C0 "&2T&.")
        return StrText3(psw, 0x4F) + " : " + SzMenuBackGlyph();
    case 851:   // 0x186E28: the current job's failure line (SW +0x4044 +0xC8: the Job Failed screen 1067)
    {
        const JOB3* pjob = g_tasks3.pjobCur;
        int idText = IdJobFailText3();   // (jobend3.h: the line JobEndFail3 was given)
        if (idText == -1) idText = pjob && pjob->plo ? int(pjob->plo->U32Field3(0, 0xC8, uint32_t(-1))) : -1;
        return StrText3(psw, idText);
    }
    case STR_PROMPT: return StrText3(psw, s_idPrompt);   // 0x25F5E8
    case STR_CARD_MESSAGE: return StrText3(psw, s_idCardMessage);   // 0x1D1A70
    case STR_CARD_PROMPT:   // 0x1D1AB0 (the strings 0x323D80.. ; texts 0x47 OK, 0x45 Yes, 0x46 No)
        if (s_kindCardPrompt == 1) return "&2S&.: " + StrText3(psw, 0x47);
        if (s_kindCardPrompt == 2 || s_kindCardPrompt == 3)
            return "&2S&. : " + StrText3(psw, 0x45) + "       " + StrText3(psw, 0x46) + " : &2T&.";
        return {};
    case STR_TITLE:   // 0x222140 (USA: one language, no names after; 0x36F790 == 1: text 0x12 alone)
        return StrText3(psw, s_fTitleProgress ? 14 : 13) + "\n" + StrText3(psw, 17);
    default:
    {
        std::string str;
        if (FStrMenuScript3(id, &str)) return str;   // (menu.cpp: the wheel, the slots' pages)
        if (FStrEpMenu3(id, &str)) return str;       // (epmenu3.cpp: the episode chooser)
        if (id >= 1449 && id <= 1465) return StrJobHelpScript(id);   // (shop.cpp: the job help 0x222488..0x222690)
        return {};
    }
    }
}

#include "cls.h"
#include "frontend.h"
#include "disc.h"
#include "lo.h"
#include "screenobj.h"
#include "screenrt.h"
#include "sm.h"
#include "sw.h"
#include "worldaccess.h"
#include "game/sly3.h"
#include "game/scptgame3.h"
#include "hudwidget.h"

#include <cstdio>
#include <cstdlib>

LO* PloScreen3(SW* psw, int oid, int oidParent)
{
    for (LO* plo : AploOfClass(psw, S3CID(0x7E)))
        if (plo->oid == oid && (oidParent == -1 || (plo->paloParent && plo->paloParent->oid == oidParent))) return plo;
    return nullptr;
}

void InitScreens3(SW* psw)
{
    if (!psw) return;
    if (std::getenv("SLY3_HUD_LIST"))
        for (LO* plo : AploOfClass(psw, S3CID(0x7E)))
        {
            const SPRITESET* pss = static_cast<const SPRITESET*>(plo);
            std::printf("screen3: oid %d (parent %d%s) cid 0x%X prio %d ext %d canvas %u x %u, %zu elements, %zu groups; root parts:", plo->oid,
                        plo->paloParent ? plo->paloParent->oid : -1, plo->fInWorld ? "" : ", not in the world", plo->Cid(), int(plo->U32Field3(0, 0x94, 0)), pss->pso ? pss->pso->fExternal : -1,
                        pss->pso ? pss->pso->ul94 : 0, pss->pso ? pss->pso->ul98 : 0, pss->pso ? pss->pso->aelem.size() : 0,
                        pss->pso ? pss->pso->agroup.size() : 0);
            if (pss->pso && !pss->pso->agroup.empty())
                for (const SPART& part : pss->pso->agroup[0].apart) std::printf(" %d", part.id);
            std::printf("\n");
            if (std::getenv("SLY3_HUD_LIST")[0] == '2' && pss->pso)   // (the text elements: kind 0 a text id, 1 a script string)
            {
                SCREENOBJ so = *pss->pso;
                std::string err;
                if (so.fExternal && so.aelem.empty()) FLoadScreenFile(&so, g_disc.ReadWorld(so.fk), err);
                for (const SELEM& e : so.aelem)
                    if (e.kind == SELEMK_Sound)
                        std::printf("screen3:   %d text elem %u: kind %d id %d font %d '%s'\n", plo->oid, e.idx, e.ab3[1], e.n3, e.ab3[2],
                                    e.ab3[1] == 0 ? StrText3(psw, e.n3).c_str() : "");
            }
        }
    // (debug: SLY3_HUD_DUMP=oid[@parent]: a screen object's elements and its group tree, keys and items)
    if (const char* sz = std::getenv("SLY3_HUD_DUMP"))
    {
        char* pe = nullptr;
        const int oid = int(std::strtol(sz, &pe, 0));
        const int oidParent = *pe == '@' ? int(std::strtol(pe + 1, nullptr, 0)) : -1;
        if (LO* plo = PloScreen3(psw, oid, oidParent); plo && static_cast<const SPRITESET*>(plo)->pso)
        {
            SCREENOBJ so = *static_cast<const SPRITESET*>(plo)->pso;
            std::string err;
            if (so.fExternal && so.aelem.empty()) FLoadScreenFile(&so, g_disc.ReadWorld(so.fk), err);
            for (const SELEM& e : so.aelem)
            {
                std::printf("dump %d: elem %u kind %u n %d", oid, e.idx, e.kind, e.n);
                if (e.kind == SELEMK_Sprite)
                    for (const SPOLY& poly : e.apoly)
                        std::printf(" [prim %u ctl %u rgba %08X nv %zu vb %02X]", poly.primitive, poly.control, poly.rgba, poly.axy.size(), poly.fVertexBytes);
                if (e.kind == SELEMK_Ref) std::printf(" ref %u", e.ref);
                if (e.kind == SELEMK_Sound) std::printf(" text kind %d id %d font %d", e.ab3[1], e.n3, e.ab3[2]);
                if (e.kind == SELEMK_Image || e.kind == SELEMK_Quad) std::printf(" tex 0x%X (%d %d %d %d)", e.idTexture, e.x0, e.y0, e.x1, e.y1);
                std::printf("\n");
            }
            for (size_t g = 0; g < so.agroup.size(); ++g)
            {
                const SGROUP& grp = so.agroup[g];
                std::printf("dump %d: group %zu set %u slots %u\n", oid, g, grp.cframeSet, grp.cframe);
                for (const SPART& part : grp.apart)
                {
                    std::printf("dump %d:   part %d len %.2f keys %zu\n", oid, part.id, part.dt, part.akey.size());
                    for (const SKEY& k : part.akey)
                    {
                        std::printf("dump %d:     t %.3f", oid, k.t);
                        for (const SITEM& it : k.aitem)
                            std::printf(" {w %04X %04X %04X c%d b%u/%u n%08X m %.2f %.2f %.2f %.2f %.1f %.1f}", it.aw[0], it.aw[1], it.aw[2], it.s6, it.b30, it.b31,
                                        uint32_t(it.n34), it.mat[0][0], it.mat[0][1], it.mat[1][0], it.mat[1][1], it.mat[2][0], it.mat[2][1]);
                        for (const auto& c : k.acolor) std::printf(" col %08X %08X", c[0], c[1]);
                        for (uint16_t w : k.aw) std::printf(" w%u", w);
                        for (uint8_t b : k.ab) std::printf(" b%u", b);
                        for (const auto& t : k.atag) std::printf(" tag %u:%d", t.first, t.second);
                        std::printf("\n");
                    }
                }
            }
        }
    }
    // (debug: SLY3_SUBTITLE=id[:rrggbbaa] puts a dialog line's subtitle up, as a line playing would: script string 844)
    if (const char* sz = std::getenv("SLY3_SUBTITLE"))
    {
        char* pe = nullptr;
        g_dialogsub.id = int(std::strtol(sz, &pe, 0));
        if (*pe == ':') g_dialogsub.rgba = uint32_t(std::strtoul(pe + 1, nullptr, 16));
    }
    // (debug: SLY3_CARDMSG=text[:prompt kind] the card dialog's message, as the card would set it: 847 / 848)
    if (const char* sz = std::getenv("SLY3_CARDMSG"))
    {
        char* pe = nullptr;
        const int id = int(std::strtol(sz, &pe, 0));
        SetCardMessage3(id, *pe == ':' ? int(std::strtol(pe + 1, nullptr, 0)) : 2);
    }
    // (debug: SLY3_TITLE=1 the title shown at the world's start, as the title script's ShowTitle: frontend.h)
    if (std::getenv("SLY3_TITLE")) ShowTitle();
    if (const char* sz = std::getenv("SLY3_HUDSHOW"))
        for (const char* p = sz; *p;)
        {
            char* pe = nullptr;
            const int oid = int(std::strtol(p, &pe, 0));
            if (pe == p) break;
            int oidParent = -1, idPart = -1;
            if (*pe == '@') oidParent = int(std::strtol(pe + 1, &pe, 0));
            if (*pe == ':') idPart = int(std::strtol(pe + 1, &pe, 0));
            if (LO* plo = PloScreen3(psw, oid, oidParent)) ShowScreenObj(plo, idPart);
            else std::fprintf(stderr, "screen3: no screen object %d (parent %d)\n", oid, oidParent);
            p = *pe == ',' ? pe + 1 : pe;
        }
}

// m_ext's binocucom: the frame 1038 (one under each actor), the lower panel 1039, the top 1042 (TODO(sly3): the other
// worlds' binocucoms, when their oids differ)
bool FWideEdgesScreen3(const LO* plo)
{
    if (!plo) return false;
    static const bool s_fOff = std::getenv("SLY3_NOWIDEEDGES") != nullptr;
    return !s_fOff && (plo->oid == 1038 || plo->oid == 1039 || plo->oid == 1042);
}

// The scripts' screen hooks (the script code's game/scptgame3.h, set at the start: g_scpthooks3 is constinit): the title
// (0x50: ShowTitle 0x221AC8, the wait 0x21DD30), the screen objects (0x1A / 0x1B: vt slots +0x1A8 / +0x1B0,
// FScreenObjDone 0x260738), the hint (0x60 / 0x61: the prompt widget 0x21E870 / 0x21E930) and the subtitle's buttons
// (0x4E: *(0x4BC228) = mask & 0xF0, script string 845)
namespace
{
struct INITSCREENHOOKS3
{
    INITSCREENHOOKS3()
    {
        g_scpthooks3.pfnShowTitle = [] { ShowTitle(); };
        g_scpthooks3.pfnFTitleShowing = [] { return FTitleShown(); };
        g_scpthooks3.pfnShowScreenObj = [](LO* plo, int n) {
            if (std::getenv("SLY3_SCREENHOOKPRINT")) std::printf("screen3: show %d part %d\n", plo ? plo->oid : -1, n);
            if (plo) ShowScreenObj(plo, n);
        };
        g_scpthooks3.pfnHideScreenObj = [](LO* plo) { if (plo) HideScreenObj(plo); };
        g_scpthooks3.pfnFScreenObjDone = [](LO* plo) {
            const bool f = !plo || FScreenObjDone(plo);
            if (static const bool s_fPrint = std::getenv("SLY3_SCREENHOOKPRINT") != nullptr; s_fPrint)
                std::printf("screen3: done? %d -> %d (state %d)\n", plo ? plo->oid : -1, int(f), plo ? NScreenObjState(plo) : -1);
            return f;
        };
        g_scpthooks3.pfnShowHint = [](int id, float g) { HudPrompt(id, g); };
        g_scpthooks3.pfnClearHint = [](int id) { HudPromptEnd(id); };
        g_scpthooks3.pfnSetButtonPrompt = [](int grf) { g_dialogsub.grf = uint32_t(grf) & 0xF0; };
    }
} s_initscreenhooks3;
}  // namespace
