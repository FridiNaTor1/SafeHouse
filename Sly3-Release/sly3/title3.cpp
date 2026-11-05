// Sly 3's title (title3.h).

#include "title3.h"

#include "card3.h"
#include "game3.h"

#include "../frontend.h"
#include "../savedata.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

TITLE3 g_title3;

// orig3: 0x221AC8 ShowTitle (+0x54 1, +0x50 0, the widget shown (vt +0x50) and its text (screen object vt +0x1C0))
void ShowTitle3()
{
    g_title3.n54 = 1;
    g_title3.n50 = 0;
    g_title3.fShown = true;
}

bool FTitleShowing3() { return g_title3.fShown; }

// orig3: 0x18AB08 StartGameFromSave(slot): the slot's progress (NProgress on SW +0x406C); none while the title shows:
// the game goes on in place (0x18AD58: the two pads' 0x210F60; 0x221B40 the title leaves); else the slot becomes the
// game's save (0x36DBA8, copied to 0x468D30 by 0x18AC60), 0x21D290 / 0x21D570 apply it, the task keys GAME +0x1498 /
// +0x149C / +0x14A0 are reset and the saved world (NProgress on SW +0x4074, 1 by default) loads (0x289070 with 0x1A8,
// or 0x289198).
void StartGameFromSave3(int islot)
{
    if (islot < 0 || islot >= 3) return;
    const GAMESAVE& save = g_aslot[islot].save;
    // (the progress reference's value: TODO(sly3) NProgress 0x2BC010 on the option SW +0x406C; a slot without any
    // progress is all zeros below its identity +0x6800, as the card's create writes it)
    bool fFresh = true;
    for (std::size_t i = 0; i < 0x6800 && i < sizeof(save.ab); ++i)
        if (save.ab[i]) { fFresh = false; break; }
    if (fFresh && FTitleShowing3())
    {
        std::printf("title: StartGameFromSave slot %d: no progress, the game goes on\n", islot + 1);
        g_title3.fShown = false;
        return;
    }
    g_gamesave = save;
    // TODO(sly3): 0x21D290 / 0x21D570 (the save applied), the keys reset, the saved world's transition
    std::printf("title: StartGameFromSave slot %d: a saved game (its world's transition is not ported; the game goes on)\n", islot + 1);
    g_title3.fShown = false;
}

// orig3: 0x221C08 (the part the boot's title uses: START)
void UpdateTitle3(bool fStart)
{
    TITLE3& t = g_title3;
    if (!t.fShown || t.n50 != 0 || !fStart) return;
    if (g_islotChosen >= 0)
    {
        // a save chosen: SetCardState(card, 1) and StartGameFromSave 0x18AB08(slot)
        SetCardState3(g_card3, 1);
        StartGameFromSave3(g_islotChosen);
        return;
    }
    if (g_card3.state != 0)
    {
        t.n50 = 1;  // (the card is busy: answered, the card checked again)
        SetCardState3(g_card3, 1);
        return;
    }
    t.fShown = false;  // StartNewGameFromTitle 0x221EC0 (the title leaves the HUD's list)
}

void InstallTitleGame3()
{
    static bool s_f = false;
    if (s_f) return;
    s_f = true;
    static std::vector<int> s_aframe;
    if (const char* e = std::getenv("SLY3_START_AT"))
    {
        std::stringstream ss(e);
        for (std::string tok; std::getline(ss, tok, ',');) s_aframe.push_back(std::atoi(tok.c_str()));
    }
    RegisterFrameHook3([](float) {
        bool fStart = false;
        for (int f : s_aframe) fStart = fStart || f == g_game3.cframe;
        if (g_title3.fShown) fStart = fStart || (GrfbtnTakePressed(0x800) & 0x800) != 0;
        const bool fWas = g_title3.fShown;
        UpdateTitle3(fStart);
        if (fWas && !g_title3.fShown) std::printf("title: frame %d START: the title closes\n", g_game3.cframe);
    });
}
