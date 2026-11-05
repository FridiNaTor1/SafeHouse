// Sly 3's memory card dialog on host files (card3.h). Ported from the Sly 3 asm (orig3 addresses on each function);
// the memory card thread (0x1CEEC0 / FMemcardThreadValid 0x1CEFB8) is synchronous here.

#include "card3.h"

#include "game3.h"
#include "talk3.h"

#include "../clock.h"
#include "../frontend.h"
#include "../savedata.h"
#include "../../platform/install.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <vector>

extern CLOCK g_clock3;    // game3.cpp (the game loop's clock)
void ChooseNewestSlot();  // frontend.cpp (orig 0x1AB418 (6); Sly 3's 0x1CF430)

CARD3 g_card3;

namespace
{
CARDHOST3 s_hostDefault;
CARDHOST3* s_phost = &s_hostDefault;

std::filesystem::path DirSaves() { return cane::userDirectory() / "Saves"; }

// the identity of m_ext's progress layout (+0x6800 / +0x6804 of the reference's fresh slots, written by the
// original's create thread): used when no world's layout is loaded yet
constexpr int32_t c_nLayoutMext = 0x5504, c_sumLayoutMext = 0x2D7125;
}  // namespace

void SetCardHost3(CARDHOST3* phost) { s_phost = phost ? phost : &s_hostDefault; }

int CardStatus3()
{
    std::error_code ec;
    for (int i = 1; i <= 3; ++i)
        if (std::filesystem::exists(DirSaves() / ("save" + std::to_string(i)), ec)) return 6;
    return 3;
}

bool FCreateCardSaves3(std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(DirSaves(), ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }
    // the empty block: zeros stamped with the layout's identity (ValidateGameSave 0x21C0F0 on a zero block)
    const GAMESAVE saved = g_gamesave;
    g_gamesave = GAMESAVE{};
    ValidateGameSave();
    GAMESAVE gs = g_gamesave;
    g_gamesave = saved;
    int32_t n = 0;
    std::memcpy(&n, gs.ab + CB_PROGRESS, 4);
    if (n == 0)
    {
        std::memcpy(gs.ab + CB_PROGRESS, &c_nLayoutMext, 4);
        std::memcpy(gs.ab + CB_PROGRESS + 4, &c_sumLayoutMext, 4);
    }
    for (int i = 1; i <= 3; ++i)
    {
        const std::filesystem::path p = DirSaves() / ("save" + std::to_string(i));
        if (std::filesystem::exists(p, ec)) continue;
        if (!FWriteGameSaveBlock(cane::utf8(p), gs, error)) return false;
    }
    return true;
}

// orig3: 0x1D0D30
void SetCardState3(CARD3& card, int state)
{
    if (state == card.state) return;
    card.state = state;
    card.tState = s_phost->TReal();
}

// orig3: 0x1D0D78 (the table 0x323B60 over 0x1A..0x3B)
int NCardPrompt3(int msg)
{
    switch (msg)
    {
    case 0x1A: case 0x1C: case 0x1F: case 0x22: case 0x23: case 0x27: case 0x29: case 0x2F: case 0x30: case 0x31:
    case 0x3B:
        return 1;
    case 0x1D: case 0x20: case 0x28: case 0x2D: return 2;
    case 0x1B: case 0x24: return 3;
    default: return 0;
    }
}

// orig3: 0x1D0C08 (the widget shown (vt slot +0x50) unless it is already; its text set (screen object +0x34, vt slot
// +0x1C0))
void ShowCardMessage3(CARD3& card, int msg)
{
    if (card.msg != msg)
    {
        card.msg = msg;
        card.tShown = s_phost->TReal();
    }
    card.fShown = true;
    s_phost->ShowMessage(msg, NCardPrompt3(msg));
}

// orig3: 0x1D0C88
void CloseCardMessage3(CARD3& card)
{
    card.msg = -1;
    card.fShown = false;
    s_phost->HideMessage();
}

bool FCardMessageShown3(const CARD3& card) { return card.fShown; }

namespace
{
// orig3: 0x1D1370 CardYes
void CardYes3(CARD3& card)
{
    switch (card.msg)
    {
    case 0x20:  // create the Sly 3 save file (the thread 0x323BE8), "creating" meanwhile
        ShowCardMessage3(card, 0x21);
        break;
    case 0x1D:  // format (the host card is never unformatted)
        ShowCardMessage3(card, 0x1E);
        break;
    default: CloseCardMessage3(card); break;
    }
}

// orig3: 0x1D1190 CardNo (the boot's messages: closed; on 0x20 the game goes on with no save file)
void CardNo3(CARD3& card)
{
    if (card.msg == 0x20 || card.msg == 0x1D || card.msg == 0x1A || card.msg == 0x1B) card.fNoSave = true;
    CloseCardMessage3(card);
    if (card.state == 1) SetCardState3(card, 0);
}

// orig3: 0x1CF0D0 UpdateCardStatus
void UpdateCardStatus3(CARD3& card)
{
    if (s_phost->FBusy() || card.state == 3 || card.fShown) return;
    const int status = CardStatus3();  // (the status poll 0x3239F0 on the card thread)
    if (status != card.status)
    {
        card.status = status;
        card.tStatus = s_phost->TReal();
    }
    if (card.state != 1 || card.fNoSave) return;
    if (s_phost->TReal() - card.tStatus < 0.3f) return;
    switch (card.status)
    {
    case 3: ShowCardMessage3(card, 0x20); break;
    case 6:  // the slots read and the newest chosen (0x1CF640, 0x1CF430): the boot's check is done
        ReadSaveSlots("");
        ChooseNewestSlot();
        SetCardState3(card, 0);
        break;
    default: break;
    }
}

// orig3: 0x1D0DC8 (the dialog's input, then the message's own step)
void UpdateCardInput3(CARD3& card)
{
    if (!card.fShown || s_phost->FBusy()) return;
    if (s_phost->TReal() - card.tShown <= 0.1f) return;
    switch (NCardPrompt3(card.msg))
    {
    case 1:
        if (s_phost->FButtonPressed(0x80))
        {
            CardNo3(card);
            return;
        }
        break;
    case 2:
        if (s_phost->FButtonPressed(0x80))
        {
            CardYes3(card);
            return;
        }
        if (s_phost->FButtonPressed(0x10))
        {
            CardNo3(card);
            return;
        }
        break;
    default: break;
    }
    if (card.msg == 0x21)  // the folder made (the thread done at once here): checked again, or the error
    {
        std::string error;
        if (FCreateCardSaves3(error))
        {
            CloseCardMessage3(card);
            card.state = 0;
            SetCardState3(card, 1);
        }
        else
        {
            std::fprintf(stderr, "card3: the save file could not be created: %s\n", error.c_str());
            ShowCardMessage3(card, 0x22);
        }
    }
}
}  // namespace

void UpdateCard3(CARD3& card)
{
    UpdateCardStatus3(card);
    UpdateCardInput3(card);
}

namespace
{
std::vector<std::pair<int, int>> ApressParse(const std::string& press)
{
    std::vector<std::pair<int, int>> a;
    std::stringstream ss(press);
    for (std::string tok; std::getline(ss, tok, ',');)
    {
        const auto at = tok.find('@');
        if (at == std::string::npos) continue;
        const std::string b = tok.substr(0, at);
        const int grf = b == "SQUARE" ? 0x80 : b == "TRIANGLE" ? 0x10 : b == "CROSS" ? 0x40 : b == "CIRCLE" ? 0x20 : 0;
        a.emplace_back(std::atoi(tok.c_str() + at + 1), grf);
    }
    return a;
}

struct GAMECARDHOST3 final : CARDHOST3
{
    std::vector<std::pair<int, int>> apress;
    int cframeResume = -1;  // the clock restarts this many frames after the message closes (the reference: 19)
    float TReal() override { return g_clock3.tReal; }
    bool FButtonPressed(int grf) override
    {
        for (auto& [f, b] : apress)
            if (f == g_game3.cframe && (b & grf)) return true;
        return (GrfbtnTakePressed(uint32_t(grf)) & uint32_t(grf)) != 0;
    }
    void ShowMessage(int msg, int prompt) override
    {
        std::printf("card: frame %d message %d (prompt %d)\n", g_game3.cframe, msg, prompt);
        SetClockFactor(&g_clock3, 0, 0.0f);
        cframeResume = -1;
        talk3::ShowCardMessage(msg, prompt);
    }
    void HideMessage() override
    {
        std::printf("card: frame %d message closed\n", g_game3.cframe);
        cframeResume = g_game3.cframe + 19;
        talk3::HideCardMessage();
    }
};
GAMECARDHOST3 s_hostGame;
}  // namespace

void InstallCardGame3()
{
    static bool s_f = false;
    if (s_f) return;
    s_f = true;
    if (const char* e = std::getenv("SLY3_CARD_PRESS")) s_hostGame.apress = ApressParse(e);
    SetCardHost3(&s_hostGame);
    RegisterFrameHook3([](float) {
        UpdateCard3(g_card3);
        if (s_hostGame.cframeResume >= 0 && g_game3.cframe >= s_hostGame.cframeResume)
        {
            SetClockFactor(&g_clock3, 0, 1.0f);
            s_hostGame.cframeResume = -1;
        }
    });
}

int CardTest3Command(const std::string& press, int cframe)
{
    struct TESTHOST final : CARDHOST3
    {
        int frame = 0;
        std::vector<std::pair<int, int>> apress;  // (frame, button bits)
        float TReal() override { return float(frame) / 60.0f; }
        bool FButtonPressed(int grf) override
        {
            for (auto& [f, b] : apress)
                if (f == frame && (b & grf)) return true;
            return false;
        }
        void ShowMessage(int msg, int prompt) override { std::printf("%5d message 0x%X (text %d) prompt %d\n", frame, msg, msg, prompt); }
        void HideMessage() override { std::printf("%5d message closed\n", frame); }
    } host;
    host.apress = ApressParse(press);
    SetCardHost3(&host);
    std::printf("card3: slots in %s (status %d)\n", cane::utf8(DirSaves()).c_str(), CardStatus3());
    CARD3& card = g_card3;
    card = CARD3{};
    SetCardState3(card, 1);  // the boot's kind 5
    int stateLast = card.state;
    for (; host.frame < cframe; ++host.frame)
    {
        UpdateCard3(card);
        if (card.state != stateLast)
        {
            std::printf("%5d card state %d\n", host.frame, card.state);
            stateLast = card.state;
        }
    }
    for (int i = 0; i < 3; ++i)
        std::printf("slot %d: %s%s\n", i + 1, g_aslot[i].fFile ? "read " : "none ", g_aslot[i].path.c_str());
    std::printf("card3: status %d, chosen slot %d%s\n", CardStatus3(), g_islotChosen, card.fNoSave ? ", no save file (declined)" : "");
    SetCardHost3(nullptr);
    return 0;
}
