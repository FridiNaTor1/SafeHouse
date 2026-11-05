#pragma once

// Playback of a screen object's group tree (Sly 2's 2D "movie clips", module 0x217BA0..0x21A998): each group has
// entries (playing instances) with display slots; an entry plays one of the group's parts (a timeline of keys);
// crossing a key places items into slots (an element, its 2x3 transform and colours, interpolated to the item's
// next key), and a slot holding a kind-4 element plays a nested group's entry. Drawing (0x213300) walks the
// slots depth first: sprites, text boxes, textured quads, with the mask brackets of linked slots.

#include "screenobj.h"

#include <array>
#include <string>
#include <vector>

struct SW;
struct FONTBRX;

struct SSLOT
{
    const SELEM* pelem = nullptr;   // +0x30 the piece
    std::array<float, 6> m{1, 0, 0, 1, 0, 0};   // +0x10 the drawn 2x3 (with the parent's)
    uint32_t c0 = 0x7F7F7F7F, c1 = 0xFFFFFFFF;   // +0x00 / +0x04 the drawn colours (offset / scale, 127-biased)
    // from the keys
    float t0 = 0, t1 = 0;           // +0x38 / +0x48
    const SITEM* pit0 = nullptr;    // +0x44
    const SITEM* pit1 = nullptr;    // +0x54
    uint32_t kc0 = 0x7F7F7F7F, kc1 = 0xFFFFFFFF, nc0 = 0x7F7F7F7F, nc1 = 0xFFFFFFFF;   // +0x3C/+0x40, +0x4C/+0x50
    uint8_t fInterp = 0, b61 = 0;   // +0x60 / +0x61
    int mask = 0;                   // +0x34: 1 opens a masked section, 4 closes it
    int ientrySub = -1;             // +0x58 the nested group's entry (kind 4)
    int idItem = 0;                 // +0x64 the placing item's identity (SITEM +0x34: the rows 0x214100 finds)
    const void* prtgtOwner = nullptr;   // kind 6: the object whose render target the quad shows (elem +0x14)
    float tCached = -1.0f;          // +0x5C the time it was last placed at (0x219FE0's phase-0 pass releases the others)
};

struct SENTRYRT
{
    int ipart = -1;                 // +0x08
    int ikey = 0;                   // +0x0A
    float t = 0;                    // +0x0C
    bool fTrig = false;             // +0x10: held (a key's trigger 7, 0x218748, or a seek 0x218060): drawn, not advanced
    bool fUsed = false;
    std::vector<SSLOT> aslot;
};

struct SGROUPRT
{
    std::vector<SENTRYRT> aentry;
    int ipartSel = -1;              // +0x20: the part the next nested start plays (0x213B00 without a playing entry)
    int ipartStopped = 0;           // +0x1C / +0x18 / +0x10: the part an entry stopped at, its time and frame
    float tStopped = 0;             // (0x218660; a start in the same frame resumes it: 0x217CA8)
    int64_t frameStopped = -1;
    bool fMat = false;              // +0x2C: a transform the group's entries are drawn through (0x213CD8)
    std::array<float, 6> mat{1, 0, 0, 1, 0, 0};
};
extern int64_t g_cframeScreen;      // the screen objects' frame count (orig 0x2DDF18)

class CScreenPlayer
{
public:
    void Init(const SCREENOBJ* pso);
    // orig: 0x00213830 (starts the root group's part with this id on entry 0)
    bool FStartPart(int idPart);
    bool FStartPartIndex(int ipart);
    // orig: 0x002183E8 + 0x00219FE0 (advances the root entry by dt and places its slots)
    void Update(float dt);
    // orig: 0x00213300 (draws the visible object): into g_hud; text from the world's strings
    void Draw(const FONTBRX* pfont, const FONTBRX* pfontIcons, const SW* psw);
    // the owner's 2x3 transform's translation (SCREENOBJ +0xF8 +0x10 / +0x14, e.g. 0x20F7E0's stacking of the counters)
    float dxOwner = 0.0f, dyOwner = 0.0f;
    // the part currently playing has reached its end (or its trigger)
    bool FPartDone() const;
    // orig: 0x00217BA0 (the root's entry 0 restarted on the part with id k; 0x314 or an unknown id: part 0)
    void StartRootPart(int idPart);
    // orig: 0x00218638 (the root's entry 0 stopped)
    void StopRoot();
    // orig: 0x002121E8 (the group having part id k is busy: entry 0 plays a part other than 0, not held; 0x314:
    // the root's entry 0 not held)
    bool FGroupBusy(int idPart) const;
    // orig: 0x00212E40 (every group having part id k: its last playing entry held at the fraction g of its part)
    void SetPartFraction(int idPart, float g);
    // orig: 0x00213B00 (the group having part id k plays it: its playing nested entry restarted on it, else selected
    // for its next start); fReset: 0x00213928 (restarted on its part 0, else nothing selected)
    void SelectPart(int idPart, bool fReset = false);
    // orig: 0x00213BD8 / 0x00213A08: the slot of identity `match` linking the group of part k plays it (0x214100),
    // or (every group having part k) plays its part 0
    void SelectPartAt(int idPart, int match);
    void ResetPartAt(int idPart, int match);
    // orig: 0x00213CD8: the group having part k is drawn through m (a 2x3)
    void SetPartMatrix(int idPart, const std::array<float, 6>& m);
    // (Sly 3's RAM checks, sly3/hud3.cpp) entry ientry of group igroup started on part ipart and placed at time t
    void SeedEntry(int igroup, int ientry, int ipart, float t);
    // the root plays something
    bool FActive() const { return !agroup.empty() && !agroup[0].aentry.empty() && agroup[0].aentry[0].ipart >= 0; }

    const SCREENOBJ* pso = nullptr;
    std::vector<SGROUPRT> agroup;
    std::array<float, 6> mRoot{1, 0, 0, 1, 0, 0};

private:
    void StartEntry(int igroup, int ientry, int ipart);
    void ReleaseSlot(SSLOT& slot);
    void Advance(int igroup, int ientry, const std::array<float, 6>& v, uint32_t pc0, uint32_t pc1, float dt);
    void CrossKey(int igroup, int ientry, int ikey, const std::array<float, 6>& v, uint32_t pc0, uint32_t pc1);
    void PlaceSlots(int igroup, int ientry, const std::array<float, 6>& v, uint32_t pc0, uint32_t pc1, float dt);
    struct LEAF { const SSLOT* pslot; int igroup = -1, ientry = -1, islot = -1; };
    void Collect(int igroup, int ientry, std::vector<LEAF>& aleaf) const;
    bool FFindGroupPart(int idPart, int& igroup, int& ipart) const;
    SSLOT* PslotLinking(int igroup, int ientrySub, int igroupIn, int ientryIn);
    void SeekEntry(int igroup, int ientry, float t);
    int IpartFirstForNested(int igroup, float* pt);
    bool FRestartAt(int igroup, int ipart, int match, int igroupIn, int ientryIn);
};

// The game's script-built strings (0x216BC8's table 0x3E1C88 {id, fn, arg}): the game loop sets this; without it
// a script string id falls back to the localized string of the same id.
extern std::string (*g_pfnScriptString)(int id);

// (Sly 3) the localized string of an id when set (the world's UTF-16 text as the font's 8-bit codes), in place of
// the SW's strings
extern std::string (*g_pfnLocalizedString)(int id);
// The script string ids drawn specially: the dialog subtitle (Sly 2 0x2EF; Sly 3 0x34C = 844, orig3 0x25F598) and the
// ticker prompt (Sly 2 0x2F1; Sly 3: none known, -1)
extern int g_idScreenSubtitle, g_idScreenTicker;

// (QoL overlays, not the original: replay.cpp's Replay Jobs) a text element about to be drawn with its draw state; the
// hook may draw more in the same style and returns true when the element itself is not to be drawn. Null: none.
struct FONTBRX;
struct TEXTDRAW
{
    const FONTBRX* pfont = nullptr;
    const FONTBRX* pfontIcons = nullptr;
    int kind = 0, id = -1;      // the element's text: 0 a localized string, 1 a script value (its id)
    float x = 0, y = 0;         // the box origin (DrawTextBox's x / y)
    float size = 0, sx = 1, sy = 1, wBox = 0, hBox = 0;
    int halign = 0;
    glm::vec4 off{0.0f}, sc{1.0f}, offIcon{0.0f};
    float depth = 0;
};
extern bool (*g_pfnTextDrawHook)(const TEXTDRAW& td);

// The localized string of an id (0x13E368 on SW+0x4640), "" when absent.
std::string StrLocalized(const SW* psw, int id);
extern float g_zScreenBase;   // the depth base of the screen object being drawn (its priority << 18)
