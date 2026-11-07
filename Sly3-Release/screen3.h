#pragma once

// Sly 3's 2D on the copied Sly 2 screen runtime: the screen objects 0x9E..0xA7 load into Sly 2's structs
// (screenobj.cpp, orig3 0x260810) and run on screenrt.cpp / splayer.cpp with Sly 3's numbers (this header). The hooks the
// scripts and the game logic use (the script code: the title / binocucom scripts' show / hide; the world flow code: the card prompt).
//
// Show / hide a screen object (an LO of class 0x9E..0xA7):
//   ShowScreenObj(plo, -1)               vt slot 53 (-1 -> slot 56): its show 886 / idle 887 / hide 888 parts
//   ShowScreenObj(plo, k)                vt slot 53: straight to part k (no show / hide part)
//   ShowScreenObjParts3(plo, s, i, h)    orig3 0x260F18 with all three parts
//   HideScreenObj(plo)                   vt slot 54 (through its hide part)
//   FScreenObjDone(plo), FScreenObjVisible(plo), SelectScreenObjPart(...), SetScreenObjArg(plo, i, textId) (screenrt.h)
// Script strings (a text element of kind 1 names one: the table orig3 0x47A4B8 {id, fn, arg} x 178): STR_* below;
// others can be supplied with SetScriptString3.

#include <functional>
#include <string>

struct LO;
struct SW;

namespace sly3ui
{
// the root parts (Sly 2's 0x30F..0x314 + 103)
constexpr int PART_SHOW = 886;   // 0x376
constexpr int PART_IDLE = 887;   // 0x377
constexpr int PART_HIDE = 888;   // 0x378
constexpr int PART_HELD = 891;   // 0x37B (the root's idle when it has no 887; a nested group's start)
// script strings (orig3 0x47A4B8)
constexpr int STR_ARG0 = 834;            // 834..843: the text of the object's argument i (+0xBC + 4 i; 0x25F4F8)
constexpr int STR_SUBTITLE = 844;        // the dialog's subtitle (0x25F598: FDialogSubtitleText on 0x4BC220)
constexpr int STR_SUBTITLE_BUTTONS = 845;// its buttons (0x25F5C0)
constexpr int STR_PROMPT = 846;          // the prompt's text (0x25F5E8: text id 0x46F788)
constexpr int STR_CARD_MESSAGE = 847;    // the card dialog's message (0x1D1A70: the card 0x3751D0 +0x50)
constexpr int STR_CARD_PROMPT = 848;     // its buttons (0x1D1AB0: NCardPrompt kind 1 "&2S&.: OK", 2 / 3 Yes / No)
constexpr int STR_TITLE = 1700;          // the title's "Press START ... / Press SELECT ..." (0x222140)
}  // namespace sly3ui

// orig3: 0x00260F18 (see above)
void ShowScreenObjParts3(LO* plo, int idShow, int idIdle, int idHide);

// (screenrt.cpp) the screen object whose texts are drawn now (its own script strings: 812 a coin counter's count)
void SetScreenDrawing3(const LO* plo);
// The prompt's text id (orig3 0x46F788; -1 none): script string 846
void SetPromptText3(int idText);
int IdPromptText3();
// The card dialog's message (the card 0x3751D0 +0x50, a text id; -1 none) and its prompt kind (NCardPrompt 0x1D0D78:
// 0 none, 1 OK, 2 / 3 yes / no): script strings 847 / 848
void SetCardMessage3(int idText, int kindPrompt);
// The title's progress choice (0x222140: text 13 "New Game" when the chosen save has no progress, else 14)
void SetTitleHasProgress3(bool f);
// Another script string (id of the table) supplied by the game logic; an empty function removes it
void SetScriptString3(int id, std::function<std::string()> fn);

// The Sly 3 strings of StrScreenScript (screenrt.cpp): *pf true when id is one of Sly 3's table (its string, "" when
// not ported yet), false for an id the table does not have.
std::string StrScreenScript3(const SW* psw, int id, const int* aarg, bool* pf);
// A world's text by id (UTF-8)
std::string StrText3(const SW* psw, int id);

// A screen object by oid (and its parent's oid when not -1: m_ext's binocucom pieces 1038..1045 have a copy under Sly
// 24 (the blue frame) and one under Bentley 25 (the green one))
LO* PloScreen3(SW* psw, int oid, int oidParent = -1);
// A world's start (InitHudWidgets): the debug shows (SLY3_HUDSHOW=oid[@parent][:part],...; SLY3_HUD_LIST=1 lists the
// world's screen objects with their parent, priority and root parts)
void InitScreens3(SW* psw);
// (display.h fWideBackgrounds: CHud::fWideEdgesObj) a full-screen frame whose border continues to a wide picture's edges
bool FWideEdgesScreen3(const LO* plo);
