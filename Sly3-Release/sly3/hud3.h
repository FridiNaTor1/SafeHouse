#pragma once

// Sly 3's screen objects drawn (the Sly 3 renderer): the HUD, the prompts, the binocucom frame and its subtitle, the
// title's "Press START". Sly 3's screen objects (classes 0x9E..0xA7, screen3.h SCREENOBJ3) are converted to Sly 2's
// structs (screenobj.h SCREENOBJ) and played / drawn by the copied player and renderer (splayer.h CScreenPlayer,
// hud.h CHud), which keeps the Phase 3 QoL hooks (promptglyph's dynamic button icons, the wide-picture handling).
// The conversion (checked against the Sly 3 asm):
// - an item (orig3 0x2631C8 / CrossKey 0x2686A0, 0x30 bytes, a packed U64 at +0): byte 0 the slot (Sly 2 aw[1]),
//   bits 8..13 the masked-section link (aw[2]), bits 14..23 the element (aw[0]), bits 24..30 the key colour (0x7F
//   none; Sly 2 s6), bit 31 interpolated to the slot's next key (b30); +4 the S32 identity (n34); +0x10 the 2x3.
// - a key: bit 24 of +4 holds the entry (0x2683F8: Sly 2's trigger 7), bits 25..27 + the S32 its event (0x268438:
//   1 a sound, 2 the sound stopped, 3 a dialog line, 4 the dialog stopped = Sly 2's kinds 0..3), the U8 list the
//   released slots (0x2685B0: Sly 2's U16 list); a colour pair is {offset, scale} in read order.
// - a text element (0x2639E0): U32 colour +0x14, F32 size +0x18, height +0x20, width +0x1C, S8 halign +0x24, S8
//   kind +0xC (0 localized, 1 a script value), S8 +0x2C, S32 id +0x10. Its UTF-16 text is drawn as the font's
//   Windows-1252 codes (the fonts' 92 characters, U+2019 / U+201C / U+201D / U+2026 included, all have one).
// - the draw (0x261C08): slots of a shown object in order, depth 16 + 256 per slot, mask brackets 1 / 4 as Sly 2, the
//   colour offset 2 x (c - 127) (hud.h g_sScreenColorOffset), the canvas 512 x 224 field lines (g_uiKy = 7 / 15,
//   origin 1792 / 1936: 0x260930 from the display 0x36DBE0 {512, 448, field divisor 2}).

#include "screen3.h"

#include "../screenobj.h"
#include "../splayer.h"
#include "../light.h"

#include <glm/glm.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

struct TEXT3;
struct SW;
struct LO;

// A converted screen object (Sly 2 structs; fonts' characters as Windows-1252 codes)
std::shared_ptr<SCREENOBJ> PsoFromScreenObj3(const SCREENOBJ3& so3);
// UTF-16 -> the fonts' 8-bit codes (Windows-1252; other characters dropped)
std::string StrFontFromUtf16(const std::u16string& str);

// The run-time state of a shown screen object (orig3 SCREENOBJ +0x48 state, +0x64 / +0x68 / +0x6C parts, +0x94
// priority; the update 0x261250).
struct SCREEN3
{
    const SCREENOBJ3* pso3 = nullptr;
    std::shared_ptr<SCREENOBJ> pso;
    CScreenPlayer player;
    int oid = -1;
    const LO* plo = nullptr;        // the live object (load3.h LOSCREEN3), null when loaded from the walk
    int oidParent = -1;             // its parent's oid (the HUD 7465 and the binocucom windows 1045 / 1038 are Sly's / Bentley's)
    int state = 1;                  // +0x48: 1 stopped, 2 idle part, 3 show part, 4 hide part
    int idShow = 886, idHide = 888, idIdle = 887;   // +0x64 / +0x68 / +0x60 (Sly 2: 0x30F / 0x311 / 0x310)
    int priority = 0;               // +0x94: drawn over lower ones
    int idTextBC = -1;              // +0xBC: the text id its script string 834 shows (0x25F4F8)
    bool fVisible() const { return state >= 2; }
};

// The HUD of a world: its screen objects (all of a walked world's, by oid; the first instance of an oid), the fonts.
// Interface for the Sly 3 player (the HUD logic, the binocucom) and the Sly 3 script port (the card prompts): find an object
// by oid, show / hide it, start or select its parts, set its priority; UpdateHud3 each frame, DrawHud3 after the scene.
struct HUD3
{
    std::vector<SCREENOBJ3> aso3;
    std::vector<std::shared_ptr<SCREENOBJ>> aso;
    std::vector<std::unique_ptr<SCREEN3>> ascreen;
    const FONTBRX* pfontText = nullptr;     // the dialog / prompt font (m_ext: oid 1061 element 0)
    const FONTBRX* pfontIcons = nullptr;    // the button icons ('&' escapes; promptglyph.h)
    std::vector<FONTBRX> afont;
    const TEXT3* ptext = nullptr;           // the world's strings (text3.h)
    // SW +0x431C (a list through +0xA4): the shown objects that hide every object of a lower priority (0x262128; the
    // binocucom's window 1038 hides the gameplay HUD 7465)
    std::vector<SCREEN3*> apsOccluding;
    int idPrompt = -1;                      // 0x46F788: the text id of the prompt (script string 846, 0x25F5E8; m_ext 792)
    std::map<int, std::string> mpidstr;     // script strings the game logic sets (SetHudString3), as UTF-8 / font codes
};
extern HUD3 g_hud3;

// Loads a walked world's top-level screen objects into g_hud3 (ptext: the world's strings for the text elements).
bool FInitHud3(const std::vector<uint8_t>& abWorld, const WORLDWALK3& walk, const TEXT3* ptext, std::string& error);
// The same from a loaded world: every LOSCREEN3 of psw at every level (children of the players included), its elements
// and groups read at its ibElems (until the Sly 3 class loaders load them into the live objects).
bool FInitHud3FromSw(const std::vector<uint8_t>& abWorld, SW* psw, const TEXT3* ptext, std::string& error);
// by oid (and the parent's oid when not -1: 1045 / 1038 have one copy under Sly 24 and one under Bentley 25)
SCREEN3* PscreenFind3(int oid, int oidParent = -1);
SCREEN3* PscreenFromLo3(const LO* plo);
// orig3: 0x260F18 (vt slot 49, Sly 2 0x2128E8): shown, through its show part when it has one (idPart -1), else on part idPart
void ShowScreen3(SCREEN3* ps, int idPart = -1);
// orig3: 0x260FE8 (vt slot 50): hidden through its hide part
void HideScreen3(SCREEN3* ps);
// Script strings (the table 0x47A4B8 {id, fn, arg} x 178, kind-1 text elements): a value the game logic provides
// (str in the fonts' 8-bit codes: StrFontFromUtf16). Built in: 1700 the title's prompt (0x222140: texts 13 + 17),
// 846 the prompt (idPrompt), 834 the object's idTextBC, 844 the dialog subtitle (sound.h g_dialogsub, splayer's).
void SetHudString3(int id, const std::string& str);
// a text id of the world's text in the fonts' codes (the Sly 3 boot to gameplay: the card's messages)
std::string StrTextOfId3(int id);
// SW +0x431C membership (see HUD3::apsOccluding)
void SetScreenOccluding3(SCREEN3* ps, bool f);
// Every frame (0x261250's state machine, the player's advance 0x2675C0)
void UpdateHud3(float dt);
// The shown objects into g_hud (Begin .. Flush) over the frame (width x height of the scene target).
void DrawHud3(int width, int height);

// A PCSX2 RAM dump's screen objects (a savestate's eeMemory.bin; SW 0x475480 +0x3020 + 12 cid lists):
// fRamSlots: each shown object drawn from its RAM slots (element, 2x3, colours, mask: exactly what 0x261C08 draws);
// else the native player started on the RAM's parts and times (each group's entries' part +8 / time +0xC) and
// placed by the native keys. Returns the objects shown; log lists them.
int CApplyHudFromRam3(const std::vector<uint8_t>& ram, bool fRamSlots, std::string& log);

// The binocucom's talking heads (orig3 0x1E8A38, Sly 2's PvRtgtOwnerOfId 0x1BEED8): a kind-6 quad's id 0x304 / 0x305
// names the nearest class-0x06 object of that oid (772 / 773: a render-target camera, 0x470 bytes, one pair under
// each actor; +0x90 / +0xA0 / +0xB0 the world rows (view, left, up), +0xC0 the position, +0x13C tan y, +0x10C 1 when
// active); its actor is drawn into a render target (rtgt.h) from it, and the quad shows the target.
// the Sly 3 player (the binocucom): SetHeadCamera3 each frame for the two windows' cameras (id 0x304 / 0x305, the actor).
struct HEADCAM3
{
    int id = -1;                    // 0x304 / 0x305
    int oidActor = -1;              // the actor drawn into the target (Bentley 25, Sly 24, ...)
    glm::vec3 pos{0.0f}, dir{1, 0, 0}, left{0, 1, 0}, up{0, 0, 1};
    float tanY = 0.273f;
    // the camera's own lights (+0x324 count, +0x328 the light objects inline; option EOPID 195): the actor in the target
    // is forward-lit by them in order (Sly 2's 0x1A4AD8 from the owner's +0x104 / +0x108), not by the world's
    std::vector<std::pair<LIGHTREC, LIGHTSEL>> alight;
};
void SetHeadCamera3(const HEADCAM3& hc);
// The targets' light sets for RelightForward(scene, lights, fUpload, AlightsHeadTargets3(worldShader)) (CRTGT entries;
// a target without lights keeps the frame's).
const LIGHTSET* AlightsHeadTargets3(const glm::vec4& worldShader);
void ClearHeadCameras3();
// Before the scene's render: the targets the quads took get their camera, and their actors' glob sets are drawn into
// them (SCENEGLOBSET::irtgt; the others back to the frame).
struct SCENE;
void BindHeadTargets3(SCENE& scene);

// --hud-dump3 W: a world's screen objects (elements by kind, text ids and strings, parts, fonts)
int HudDump3Command(const std::string& world);
