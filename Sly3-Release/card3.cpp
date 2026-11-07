// See card3.h. Ported from the Sly 3 asm (orig3 addresses on each function) and the old Sly 3 port's card3 (its
// state machine checked against the reference: the slots it creates are byte-identical to the original's).

#include "card3.h"

#include "clock.h"
#include "frontend.h"
#include "menu.h"
#include "savedata.h"
#include "screen3.h"
#include "screenrt.h"
#include "sw.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

void ChooseNewestSlot();   // frontend.cpp (orig3 0x1CF430)
extern int g_ncardMode;    // menu.cpp (Sly 2's card: 2 = off, the Sly 3 card below runs instead)

namespace
{
// the card widget (orig3 0x3751D0): +0x50 the message, +0x54 when shown, +0x58 the state, +0x5C its time
struct CARD3
{
    int state = 0;
    float tState = 0.0f;
    int msg = -1;
    float tShown = 0.0f;
    bool fShown = false;
    float tStatus = 0.0f;
    bool fNoSave = false;   // No on 0x20: the game goes on without a save file
    bool fPause = false;    // the clock stopped while a message shows (the reference: T frozen during the prompt)
    SW* psw = nullptr;
} s_card;

constexpr int OID_CARD_SCREEN = 1466;   // the card widget's screen object (+0x34): a global one in every world

float TReal() { return g_clock.tReal; }

bool FFolder() { return g_aslot[0].fFile || g_aslot[1].fFile || g_aslot[2].fFile; }

// the create thread (orig3 0x323BE8): the folder and save1..3, each an empty block stamped with the current progress
// layout's identity (ValidateGameSave on zeros: the original's fresh slots)
bool FCreateSaves3()
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(g_aslot[0].path).parent_path(), ec);
    const GAMESAVE saved = g_gamesave;
    g_gamesave = GAMESAVE{};
    ValidateGameSave();
    const GAMESAVE gs = g_gamesave;
    g_gamesave = saved;
    bool fOk = true;
    for (int i = 0; i < kSlotsCard; ++i)
    {
        SAVESLOT& sl = g_aslot[i];
        if (sl.fFile) continue;
        std::string err;
        if (FWriteGameSaveBlock(sl.path, gs, err))
        {
            sl.save = gs;
            sl.fFile = true;
            sl.tWrite = std::filesystem::last_write_time(sl.path, ec).time_since_epoch().count();
        }
        else
        {
            std::printf("card: %s\n", err.c_str());
            fOk = false;
        }
    }
    RechooseSaveSlots();
    return fOk;
}

// orig3: 0x1D0D30
void SetCardState3(int state)
{
    if (state == s_card.state) return;
    s_card.state = state;
    s_card.tState = TReal();
}

void SetPause(bool f)
{
    if (f == s_card.fPause) return;
    s_card.fPause = f;
    SetClockFactor(&g_clock, 0, f ? 0.0f : 1.0f);
}

// orig3: 0x1D1370 CardYes
void CardYes3()
{
    if (FMenuCardAnswer3(s_card.msg, true)) return;   // (the menu's: overwrite, erase, ...)
    switch (s_card.msg)
    {
    case 0x20: ShowCardMessage3(0x21); break;   // create the save file; "creating" while the thread runs
    case 0x1D: ShowCardMessage3(0x1E); break;   // format (a PC's folder is never unformatted)
    default: CloseCardMessage3(); break;
    }
}

// orig3: 0x1D1190 CardNo
void CardNo3()
{
    if (FMenuCardAnswer3(s_card.msg, false)) return;
    if (s_card.msg == 0x20 || s_card.msg == 0x1D || s_card.msg == 0x1A || s_card.msg == 0x1B)
    {
        s_card.fNoSave = true;
        g_islotChosen = -1;
    }
    CloseCardMessage3();
    if (s_card.state == 1) SetCardState3(0);
}

// orig3: 0x1CF0D0 UpdateCardStatus (the card's status: 3 no Sly 3 save file, 6 ready; a PC has no other)
void UpdateCardStatus3()
{
    if (s_card.state != 1 || s_card.fShown || s_card.fNoSave) return;
    if (TReal() - s_card.tState < 0.3f) return;
    if (FFolder())
    {
        // 6: the slots read, the newest chosen (0x1CF640, 0x1CF430): the check is over
        ChooseNewestSlot();
        SetCardState3(0);
        return;
    }
    static const bool s_fPrompt = std::getenv("SLY3_CARDPROMPT") || std::getenv("SLY2N_CARDPROMPT");
    if (s_fPrompt)
    {
        ShowCardMessage3(0x20);
        return;
    }
    // (PC, Sly 2's Q2) the original's Yes without the question and the "creating" notice
    if (FCreateSaves3())
    {
        std::printf("card: the save folder made (Q2: no question)\n");
        SetCardState3(0);
    }
    else ShowCardMessage3(0x22);
}

// orig3: 0x1D0DC8 (the dialog's input 0.1 s after the message shows, then the message's own step)
void UpdateCardInput3()
{
    if (!s_card.fShown || TReal() - s_card.tShown <= 0.1f) return;
    switch (NCardPrompt3(s_card.msg))
    {
    case 1:
        if (GrfbtnTakePressed(0x80)) { CardNo3(); return; }
        break;
    case 2:
    case 3:
        if (GrfbtnTakePressed(0x80)) { CardYes3(); return; }
        if (GrfbtnTakePressed(0x10)) { CardNo3(); return; }
        break;
    default: break;
    }
    if (s_card.msg == 0x21)   // the thread (synchronous here): done, or the error 0x22
    {
        if (FCreateSaves3())
        {
            CloseCardMessage3();
            s_card.state = 0;
            SetCardState3(1);   // checked again: finds the save
        }
        else ShowCardMessage3(0x22);
    }
}

LO* PloCardScreen() { return s_card.psw ? PloScreen3(s_card.psw, OID_CARD_SCREEN) : nullptr; }
}  // namespace

// orig3: 0x1D0D78 (the table 0x323B60 over 0x1A..0x3B)
int NCardPrompt3(int msg)
{
    switch (msg)
    {
    case 0x1A: case 0x1C: case 0x1F: case 0x22: case 0x23: case 0x27: case 0x29: case 0x2F: case 0x30: case 0x31: case 0x3B:
        return 1;
    case 0x1D: case 0x20: case 0x28: case 0x2D: return 2;
    case 0x1B: case 0x24: return 3;
    default: return 0;
    }
}

void StartCardCheck3()
{
    g_ncardMode = 2;   // (Sly 2's card logic, menu.cpp, off: this one runs)
    s_card.fNoSave = false;
    s_card.state = 0;
    SetCardState3(1);
}

// orig3: 0x1D0C08 (the widget shown unless it is; its screen object's text set)
void ShowCardMessage3(int msg)
{
    std::printf("card: message 0x%X (prompt %d)\n", msg, NCardPrompt3(msg));
    if (s_card.msg != msg)
    {
        s_card.msg = msg;
        s_card.tShown = TReal();
    }
    s_card.fShown = true;
    SetCardMessage3(msg, NCardPrompt3(msg));
    SetPause(true);
    if (LO* plo = PloCardScreen()) ShowScreenObj(plo, -1);
}

// orig3: 0x1D0C88
void CloseCardMessage3()
{
    s_card.msg = -1;
    s_card.fShown = false;
    SetPause(false);
    if (LO* plo = PloCardScreen()) HideScreenObj(plo);
}

bool FCardMessageShown3() { return s_card.fShown; }
bool FCardSettled3() { return s_card.state == 0 && !s_card.fShown; }
int IdCardMessage3() { return s_card.fShown ? s_card.msg : -1; }
float DtCardMessageShown3() { return TReal() - s_card.tShown; }

void UpdateCard3(SW* psw)
{
    if (s_card.psw != psw)
    {
        s_card.psw = psw;
        if (s_card.fShown)   // (a new world: the message again in its screen object)
            if (LO* plo = PloCardScreen()) ShowScreenObj(plo, -1);
    }
    UpdateCardStatus3();
    UpdateCardInput3();
}
