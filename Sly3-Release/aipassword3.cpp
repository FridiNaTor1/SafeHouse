// Sly 3's password check (orig3 module 0x18B638..0x18C5A4, the AI job 0xE7): a guard stops a disguised player
// (reaction 23, from the target classification 0x2544E0's kind 1: aijobs.cpp), says its challenge line, shows a
// sequence of four face buttons, and the player answers it. Right: the player's line, the guard lets the disguise pass
// from then on (+0x11BC, 0x2544E0's kind 0) and the tasks hear 0x50; wrong or too slow: the guard's line, the tasks
// hear 0x51, the disguise comes off (0x172AE8(-1)) and the reaction pursues (0x19A948 case 23).
//
// The player's side (orig3 0x1ADDD0 states 0x15 / 0x16, 0x1AD1D0 case 22: each face button pressed within 0.15 s is
// handed to the job, 0x18C5A8) is done here natively: while the job holds the player, its pad is taken (po.cpp
// g_pfnPadctlTake3) so it stands, and the presses go to the job. Lines: the world's dialog table (SW +0x4FB0, entries
// 0..4: the guard's start / challenge / pass / fail / timeout lines) and the two id bags of 0x25EC70 (+0x60 the tones
// of the shown buttons, 0x269..0x272; +0x64 the player's wrong answers, 0x273..0x279); the right answer is the
// challenge row's line (the rows D_00344FC8[episode]). Not natively: the guard state bit 0x40 (Sly 3's state 64 has
// no Sly 2 state), the proxy row lock (0x1FC118 / 0x1FC060), the talking animations (vt slots 94 / 95), the guard's own
// line table (+0x17C4), the prompt screen object (Sly +0x1F40: 0x260F18 parts 0x3B1..0x3B3 / 0x3AE..0x3B0).

#include "aipassword3.h"

#include "ai.h"
#include "aijobs.h"
#include "clock.h"
#include "guard.h"
#include "hud.h"
#include "lo.h"
#include "outfit3.h"
#include "po.h"
#include "sound.h"
#include "transition.h"
#include "xfmath.h"
#include "game/scptgame3.h"
#include "game/sly3.h"
#include "game/task3.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

float DxTextUi(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88);   // splayer.cpp

namespace
{
bool FPrint() { static const bool s_f = std::getenv("SLY3_PASSWORDPRINT") != nullptr; return s_f; }

// D_00344FC8[episode] (Venice D_00321AA0, China 0x321B40, the pirates 0x321BE0; read from the ELF): eight rows of
// {the player's right answer line, the four buttons (0x10 triangle, 0x20 circle, 0x40 cross, 0x80 square)}
using ROW = PASSWORDROW;
const ROW c_arowVenice[8] = {
    {634, {0x80, 0x40, 0x80, 0x80}}, {635, {0x20, 0x20, 0x40, 0x40}}, {636, {0x80, 0x10, 0x80, 0x10}},
    {637, {0x40, 0x40, 0x40, 0x10}}, {638, {0x20, 0x40, 0x20, 0x20}}, {639, {0x10, 0x20, 0x20, 0x10}},
    {640, {0x10, 0x10, 0x40, 0x40}}, {641, {0x20, 0x80, 0x80, 0x20}}};
const ROW c_arowChina[8] = {
    {642, {0x80, 0x20, 0x80, 0x80}}, {643, {0x10, 0x40, 0x80, 0x20}}, {644, {0x10, 0x80, 0x10, 0x20}},
    {645, {0x40, 0x80, 0x80, 0x40}}, {646, {0x40, 0x10, 0x20, 0x20}}, {647, {0x80, 0x40, 0x20, 0x40}},
    {648, {0x40, 0x40, 0x40, 0x40}}, {649, {0x80, 0x20, 0x10, 0x40}}};
const ROW c_arowPirate[8] = {
    {650, {0x10, 0x20, 0x80, 0x40}}, {651, {0x80, 0x10, 0x20, 0x20}}, {652, {0x40, 0x20, 0x40, 0x10}},
    {653, {0x80, 0x80, 0x10, 0x80}}, {654, {0x40, 0x20, 0x40, 0x20}}, {655, {0x80, 0x10, 0x20, 0x10}},
    {656, {0x40, 0x80, 0x20, 0x10}}, {657, {0x20, 0x20, 0x20, 0x20}}};
// orig3 0x18B658: the row of the challenge line's index (the episode's table; -1: Venice's)
const ROW* ProwOf(int i)
{
    if (i < 0 || i >= 8) return nullptr;
    switch (IepisodeCur())
    {
    case -1: case sly3::EP_Venice: return &c_arowVenice[i];
    case sly3::EP_China: return &c_arowChina[i];
    case sly3::EP_Pirate: return &c_arowPirate[i];
    default: return nullptr;
    }
}

// orig3 0x25EC70's two id bags (0x2BF8A0 over the ids; vt slot 1 the next one: each once per round, a round not
// starting with the last one)
struct BAG
{
    std::vector<int> aid;
    std::vector<int> aorder;
    size_t inext = 0;
    int ilast = -1;
    int IdNext()
    {
        const int c = int(aid.size());
        if (inext >= aorder.size())
        {
            aorder.resize(size_t(c));
            for (int k = 0; k < c; ++k) aorder[size_t(k)] = k;
            for (int k = c - 1; k > 0; --k) std::swap(aorder[size_t(k)], aorder[size_t(int(GRandInRange(0.0f, float(k) + 0.999f)))]);
            if (c > 2 && aorder[0] == ilast) std::swap(aorder[0], aorder[size_t(1 + int(GRandInRange(0.0f, float(c - 1) - 0.001f)))]);
            inext = 0;
        }
        ilast = aorder[inext++];
        return aid[size_t(ilast)];
    }
};
BAG s_bagTones{{0x269, 0x26A, 0x26B, 0x26C, 0x26D, 0x26E, 0x26F, 0x270, 0x271, 0x272}};   // (+0x60)
BAG s_bagWrong{{0x273, 0x274, 0x275, 0x276, 0x277, 0x278, 0x279}};                        // (+0x64)

// The job holding the player (the player's +0x1454 is its guard)
JOBPASSWORD3* s_pjobHold = nullptr;
int s_cPassed = 0;   // (routes: the checks passed so far)

// D_00321A80 / D_00321A90: where the shown buttons (state 3) and the answer (states 5 / 6) are drawn
const glm::vec2 c_posShown(0.55f, -0.62f), c_posAnswer(0.3f, 0.7f);
}  // namespace

JOBPASSWORD3::~JOBPASSWORD3() { if (s_pjobHold == this) s_pjobHold = nullptr; }

// orig3 0x18B6C0 (slot 7)
void JOBPASSWORD3::Init()
{
    AIJOB::Init();
    st = -1;
    stFace = -1;
    idLine = -1;
}

// orig3 0x18B700 (slot 8): the check starts, the guard turns to the player (0x18BF48 mode 0)
void JOBPASSWORD3::Enter()
{
    AIJOB::Enter();
    SetState(0);
    SetFace(0);
}

// orig3 0x18B780 (slot 9)
void JOBPASSWORD3::Exit()
{
    AIJOB::Exit();
    SetState(-1);
    SetFace(-1);
}

// orig3 0x18B7F0 (slot 10): the check (0x18B828), then the facing (0x18BEE8: the turn's move done -> stand)
void JOBPASSWORD3::Update()
{
    AIJOB::Update();
    int stSeen;
    do
    {
        stSeen = st;
        switch (st)
        {
        case 0:
            if (g_clock.t - tSt > 0.1f) SetState(1);
            break;
        case 1:
            if (FLineDone()) SetState(2);
            break;
        case 2:
            if (FLineDone()) SetState(3);
            break;
        case 3:
            if (g_clock.t - tShown > 0.33f)
            {
                ++cShown;
                tShown = g_clock.t;
                if (cShown == 4) tShown = g_clock.t + 0.25f;
                else if (cShown >= 5) SetState(4);
            }
            break;
        case 4:
            SetState(5);   // (0x260738 on the prompt screen object: none natively)
            break;
        case 5:
        {
            int i = 0;
            for (; i < cAnswer; ++i)
                if (!prow || aAnswer[size_t(i)] != prow->abtn[size_t(i)]) break;
            if (i < cAnswer) SetState(6);
            else if (cAnswer >= 4)
            {
                fPassed = 1;
                SetState(6);
            }
            else if (g_clock.t - tAnswer > 0.75f) SetState(12);
            break;
        }
        case 6:
            if (g_clock.t - tSt > 0.5f) SetState(fPassed ? 7 : 9);
            break;
        case 7:
            if (FLineDone()) SetState(8);
            break;
        case 9:
            if (FLineDone()) SetState(10);
            break;
        case 8: case 10: case 12:
            if (FLineDone()) SetState(-1);
            break;
        case 11:
            SetState(-1);
            break;
        }
    } while (stSeen != st);
    do
    {
        stSeen = stFace;
        if (stFace == 0 && pchild && static_cast<JOBMOVE*>(pchild)->st == 1) SetFace(1);
    } while (stSeen != stFace);
}

// orig3 0x18C1A8: the line playing stopped
void JOBPASSWORD3::StopLine()
{
    if (idLine == -1) return;
    StopDialog();
    idLine = -1;
}

// orig3 0x18C0F8: a line (with its speaker: the guard, the player or none)
void JOBPASSWORD3::PlayLine(int id, LO* ploSpeaker)
{
    idLine = id;
    if (id == -1) return;
    CSndVoice* p = PsndStartDialog(id, 0);
    if (p && ploSpeaker) p->SetSource(ploSpeaker);
    if (FPrint()) std::printf("password: guard %d line %d%s\n", pguard->palo->oid, id, p ? "" : " (none)");
}

// orig3 0x18C018: the guard's line of the world dialog table's entry i (its list by +0x2F0); entry 1 picks the row
void JOBPASSWORD3::PlayGuardLine(int i)
{
    int index = -1;
    const int id = IdWorldDialog3(i, GuardField3(pguard, 0x2F0, 0) != 0, &index);
    if (id == -1) return;
    if (i == 1) prow = ProwOf(index);
    PlayLine(id, pguard->palo);
}

// orig3 0x18C170: the line is over (FDialogPlaying)
bool JOBPASSWORD3::FLineDone() const { return idLine == -1 || !FDialogPlaying(idLine); }

// orig3 0x18BB20
void JOBPASSWORD3::SetState(int n)
{
    if (n == st) return;
    StopLine();
    const int old = st;
    st = n;
    tSt = g_clock.t;
    if (FPrint()) std::printf("password: guard %d state %d -> %d t %.3f\n", pguard->palo->oid, old, n, g_clock.t);
    switch (n)
    {
    case 0:
        s_pjobHold = this;   // (the player's +0x1454 = the guard; Sly's state 0x15)
        break;
    case 1:
        PlayGuardLine(0);    // (Sly's state 0x16)
        break;
    case 2:
        PlayGuardLine(1);
        break;
    case 3:
        PlayLine(s_bagTones.IdNext(), nullptr);
        tShown = g_clock.t + 0.33f;   // (0x344FEC)
        break;
    case 5:
        tAnswer = g_clock.t + 0.75f;
        break;
    case 7:
        PlayLine(prow ? prow->idLine : -1, ploTarget);
        break;
    case 8:
        ++s_cPassed;
        pguard->fPassed11BC = 1;
        pguard->tLine11C4 = g_clock.t;
        fPassed = 1;
        PlayGuardLine(2);
        break;
    case 9:
        PlayLine(s_bagWrong.IdNext(), ploTarget);
        break;
    case 10:
        PlayGuardLine(3);
        break;
    case 12:
        PlayGuardLine(4);
        break;
    case -1:
        if (s_pjobHold == this) s_pjobHold = nullptr;   // (the player's +0x1454 = 0; Sly back to state 0)
        LockJobDone();
        switch (old)
        {
        case 8:
            Broadcast3(0x50, pguard->palo);
            break;
        case 10: case 12:
            Broadcast3(0x51, pguard->palo);
            if (ploTarget) FWearOutfit3(ploTarget, -1);   // (0x172AE8(Sly +0x1108, -1): the disguise off)
            break;
        }
        break;
    }
}

// orig3 0x238AB8 (the job marked done: the reaction's 0x238AD0 sees it)
void JOBPASSWORD3::LockJobDone() { fDone = 1; }

// orig3 0x18BF48: 0 the turn to the player (0xC9 with the alerted animation: Sly 3's 0x11, the move's native 0xB),
// 1 standing facing it (0xCA)
void JOBPASSWORD3::SetFace(int n)
{
    if (n == stFace) return;
    EndChildJob(&pchild);
    stFace = n;
    switch (n)
    {
    case 0:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pguard, 0x9D));
        o->ploFace = ploTarget;
        o->nAnim = 0xB;
        o->Enter();
        break;
    }
    case 1:
    {
        auto* o = static_cast<JOBMOVE*>(PjobPush(pguard, 0x9E));
        o->ploFace = ploTarget;
        o->Enter();
        break;
    }
    }
}

// orig3 0x18C5A8: an answer button (in states 4 / 5, at most four)
void JOBPASSWORD3::AddAnswer(int btn)
{
    if ((st == 4 || st == 5) && cAnswer < 4)
    {
        aAnswer[size_t(cAnswer++)] = btn;
        tAnswer = g_clock.t;
        if (FPrint()) std::printf("password: answer %d = 0x%X\n", cAnswer, btn);
    }
}

// orig3 0x1AD1D0 case 22 / 0x1ADDD0 states 0x15 / 0x16 (natively: the held player's pad, po.cpp)
bool FTakePadForPassword3(LO* ploPlayer, uint32_t grfPressed)
{
    JOBPASSWORD3* p = s_pjobHold;
    if (!p || p->ploTarget != ploPlayer) return false;
    if (p->st >= 1)
        for (int btn : {0x10, 0x80, 0x40, 0x20})   // (D_003225F0)
            if (grfPressed & uint32_t(btn)) p->AddAnswer(btn);
    return true;
}

LO* PloPasswordGuard3(LO* ploPlayer)
{
    return s_pjobHold && s_pjobHold->ploTarget == ploPlayer && s_pjobHold->pguard ? s_pjobHold->pguard->palo : nullptr;
}

void ResetPassword3() { s_pjobHold = nullptr; }

int CPasswordsPassed3() { return s_cPassed; }

int NPasswordButtonWanted3(LO* ploPlayer)
{
    const JOBPASSWORD3* p = s_pjobHold;
    if (!p || p->ploTarget != ploPlayer || !p->prow || (p->st != 4 && p->st != 5) || p->cAnswer >= 4) return 0;
    return p->prow->abtn[size_t(p->cAnswer)];
}

// orig3 0x18C510 (slot 15) -> 0x18C428 / 0x18C220: the buttons in a diamond, 75 apart, the asked one big (0.6) with
// its letter, the others "o" (0.3)
void DrawPasswordButtons3(const FONTBRX* pfont, const FONTBRX* picons)
{
    const JOBPASSWORD3* p = s_pjobHold;
    if (!p || !pfont) return;
    auto draw = [&](int cLimit, const int* abtn, int c, glm::vec2 pos) {
        float x = pos.x * 320.0f + 320.0f - 175.0f;
        const float y = pos.y * -240.0f + 240.0f;
        for (int i = 0; i < c; ++i, x += 75.0f)
        {
            if (i >= cLimit) continue;
            static const struct { int btn; char ch; float dx, dy; } s_akey[4] = {   // (D_00321A38)
                {0x10, 'T', 0.0f, -1.0f}, {0x20, 'O', 1.0f, 0.0f}, {0x40, 'X', 0.0f, 1.0f}, {0x80, 'S', -1.0f, 0.0f}};
            const float sSmall = 0.3f, sBig = 0.6f;
            const float dx = DxTextUi(*pfont, picons, "&2o&.", sSmall), dy = sSmall * 51.2f;
            const float dxBig = DxTextUi(*pfont, picons, "&2T&.", sBig), dyBig = sBig * 51.2f;
            for (const auto& k : s_akey)
            {
                float xk = x + k.dx * dx, yk = y + k.dy * dy, scale = sSmall;
                char ch = 'o';
                if (k.btn == abtn[i])
                {
                    xk -= (dxBig - dx) * 0.5f;
                    yk -= (dyBig - dy) * 0.5f;
                    ch = k.ch;
                    scale = sBig;
                }
                g_hud.DrawTextBox(*pfont, picons, std::string("&2") + ch + "&.", xk - 320.0f, yk - 246.4f, scale * 51.2f,
                                  1.0f, 1.0f, 640.0f, 492.8f, 0, 0, glm::vec4(128.0f, 128.0f, 128.0f, 0.0f), glm::vec4(1.0f),
                                  glm::vec4(128.0f, 128.0f, 128.0f, 0.0f), 0.0f);
            }
        }
    };
    switch (p->st)
    {
    case 3:
        if (p->prow) draw(p->cShown, p->prow->abtn.data(), 4, c_posShown);
        break;
    case 5: case 6:
        draw(4, p->aAnswer.data(), p->cAnswer, c_posAnswer);
        break;
    }
}
