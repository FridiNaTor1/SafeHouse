#pragma once

// PC addition: the map editor's cutscene editor, game side (notes/CUTSCENES_2026-11-18.md, docs/MODDING.md "Cutscenes").
//
// A Sly 2 cutscene is a script object (SCPT, cid 0x72, scpt.cpp): its record's method options build a list of
// commands, one per option (the builders 0x106A80..0x10A6D8). Command 0 opens a block and 1 closes it; a block runs as
// a "runner" (a track of the timeline), its commands one after the other, each waiting until it is done unless its
// "no waiting" word is set; a block inside a block is a track of its own that starts at that point and runs beside
// it. So the timeline is made of tracks of commands: what each command does and how long it takes (a wait's seconds, a
// line's length, an animation's length, a walk until the actor arrives ...).
//
// This file: the commands' catalogue (names, what each argument means, which track they belong to), the builders'
// argument layout (scpt.cpp), and the game-run tools the editor starts in a hidden process:
//   --cutscene-trace OID [--cutscene-out F.json] [--cutscene-at FRAME]: the world's script OID started at FRAME (60)
//       and traced each step until it stops: each track's commands with the time they started / ended, the camera, the
//       objects that moved (position / heading), the animations that played, the dialogue lines, the objects shown /
//       hidden. The editor's timeline takes its clips' times and its scrubbing from it (a cutscene plays the same
//       every time, but for idle animations).
//   --play-scpt OID[@SECONDS]: the game (windowed) with the script started a second into the world, fast-forwarded
//       to SECONDS of it first (the editor's "play from the playhead").
//   --record-session F.json: a play session recorded (the player's position, heading, speed, state, animation and the
//       pad each step), for the editor to turn into cutscene tracks (a path, a walk / run, its animations, its timing).

#include <cstdint>
#include <string>
#include <vector>

struct LO;
struct SW;
struct FONTBRX;

// ---- scpt.cpp's side

// Where a builder (the method option's thunk) puts its argument j: ib the command's slot (0x10.. as SCPTCMD's words),
// -2 its "no waiting" word, -1 not stored; otyp the argument's type (1 a float, 2 a vector, 0x8006 an int list, else an
// int); fRad: a vector of angles given in degrees and stored in radians.
struct SCPTARGSLOT
{
    int otyp = 0;
    int ib = -1;
    bool fRad = false;
};
bool FScptBuilderLayout(uint32_t pfnThunk, int& kind, std::vector<SCPTARGSLOT>& a);
// The loaded script's commands (the opening command 0 first, the closing 1 once started): kind, "no waiting", the 18 words
int CScptCommands(LO* pscpt);
bool FScptCommand(LO* pscpt, int ic, int& kind, int& b, int32_t an[18]);
// a running script's own time (-1: not running)
float TScptOf(LO* pscpt);
// (the trace's hook: ev 0 a runner made for the block opened at ic (prunParent: who made it), 1 the runner executes
// command ic, 2 the runner retired)
extern void (*g_pfnScptTrace)(LO* pscpt, const void* prun, int ev, int ic, const void* prunParent);

namespace cut
{
// The camera moves' easing curves (cplcy.cpp CPTIMED: s(u) = a u^3 + b u^2 + c u). ICURVE_EASEOUT is the PC's ease
// out, a value the original's data never uses (its 3075 camera moves: 0..3; the original takes every value past 5 as
// 5, and so does the port for all but this one)
constexpr int ICURVE_EASEOUT = 1000;
const char* SzEase(int icurve);
float SEase(int icurve, float u);

// The timeline's track kinds
enum TRACKK : int
{
    TK_Flow,      // blocks, waits, alarms, labels, pauses
    TK_Camera,
    TK_Actor,     // characters and objects moved / animated
    TK_Dialog,
    TK_Sound,     // sounds and music
    TK_Effects,   // shown / hidden objects, fades, screens, rumble, explosions, hints
    TK_Events,    // tasks, scripts, state machines, splice events
    TK_Present,   // the presentation: the letterbox (the game paused with the subtitle box), title cards, captions
                  // (hints), screens, fades (a fade object started by the script's event)
    TK_Max
};
const char* SzTrack(TRACKK tk);

// An argument's meaning (the editor's field for it)
enum PARK : int
{
    PK_Int, PK_Float, PK_Bool, PK_Seconds, PK_Distance, PK_Speed, PK_Stick, PK_Fov,
    PK_Object,     // an object id (oid; -1: none / the track's object)
    PK_Actor,      // the character it acts on (an oid; -1: the track's object)
    PK_Script,     // a script object's oid
    PK_Anim,       // an animation (ASEG) oid
    PK_Path,       // a path object's oid
    PK_Task, PK_TaskState,
    PK_Line,       // a dialogue line (its id: the subtitle's text id)
    PK_Sound,      // a sound effect id
    PK_Music,
    PK_Text,       // a text id
    PK_Block,      // a block's id (its opening command's object id)
    PK_Alarm, PK_Label, PK_Button, PK_Flags,
    PK_Pos,        // a vector (x, y, z)
    PK_Angles,     // a vector of angles (degrees in the record)
    PK_Ease,       // a camera move's easing curve (cplcy.cpp's timed curves: 0 linear, 1 ease in, 3 ease in / out; 1000 ease out)
    PK_Max
};

struct PARDESC
{
    int ib;              // the command's slot (0x10..; -2: the "no waiting" word)
    const char* name;
    PARK k;
    const char* tip;
};

struct CMDDESC
{
    int kind = -1;
    const char* name = "";      // "Walk to"
    const char* verb = "";      // "walk" (the text form's word)
    TRACKK tk = TK_Flow;
    int ibActor = -1;           // the slot naming the object it acts on (-1: the track's default object)
    int wait = 0;               // 0 done at once, 1 waits unless "no waiting" is set, 2 always waits (until done)
    std::vector<PARDESC> apar;
    const char* tip = "";
};
const CMDDESC& DescOfKind(int kind);
const std::vector<CMDDESC>& AllCommands();
// the slot the runner looks its object up by (scpt.cpp ExecuteRun's id switch; -1: the default object)
int IbObjectOfKind(int kind);

// The builders as the world's records use them: a method option (EOPID) whose function builds a command
struct BUILDER
{
    int eopid = -1;
    uint32_t pfn = 0;
    int kind = -1;
    int cArgs = 0;                       // its values in the record
    std::vector<SCPTARGSLOT> alay;       // where each value goes in the command
};
const std::vector<BUILDER>& Builders();
const BUILDER* PbuilderOfEopid(int eopid);
// (0x24's two builders: variant 0 walks (+0x24 = 1), 1 runs)
const BUILDER* PbuilderOfKind(int kind, int variant = 0);

// ---- the cutscene rules of the loaded mod packages (mod.json "cutscenes", platform/mod_package.h)
// scpt.cpp StartScpt: false: the script does not start now (its once / task condition); a script that starts with a
// "once" rule is marked seen in the save's side file (modsave.h)
bool FCutsceneMayStart(LO* pscpt);
// scpt.cpp SceneSkipKind: the rule's skip choice over the game's own class k (SKIPK_*)
int SkipKindByRule(LO* pscpt, int k);
// scpt.cpp IdScptCutscene: the script's rule says never skippable (the scenes it starts neither)
bool FCutsceneNeverSkip(LO* pscpt);
// scpt.cpp: a runner reached label n (command 0x13): a skip stops there when the script's rule says so
void NoteScptLabel(LO* pscpt, int n);
// skipcut.cpp: a skip should stop now (a rule's label reached within the last steps)
bool FCutsceneSkipStops();

// ---- (SafeHouse addition) the custom presentation (cutpresent.cpp): only for a script whose rule has
// "custom_presentation" (the cutscene editor's toggle; the original game's scripts never have a rule)
// a hint command's (0x53) "no waiting" word with this bit (0x20 / 0x21, stored as a 32-bit int: a bool value gives
// only 0 / 1): a title card of the hint's text (the original's 212 hints all have the bool 1; scpt.cpp's builder keeps
// the bit in the command's +0x18)
constexpr int SCPTB_TITLECARD = 0x20;
bool FCustomPresentation(LO* pscpt);
// scpt.cpp 0x53 / 0x54 of such a script: a caption (in the scene's pause: the subtitle box or a caption box of its
// own; else the game's hint box), its end, a title card of the text
void ShowCutCaption(int idText, float dt);
void EndCutCaption(int idText);
void ShowCutTitle(int idText, float dt);
// hudwidget.cpp: a world's HUD reset
void ResetCutPresent();
// splayer.cpp: the subtitle box's text is the caption (shown, the box up, no line speaking)
bool FCutCaptionInSubtitleBox(std::string* pstr);
// screenrt.cpp DrawScreenObjs: the caption box and the title card over the screen objects
void DrawCutPresent(const FONTBRX* pfont, const FONTBRX* picons);

// ---- the game-run tools (main.cpp / game.cpp)
// main.cpp: the tools' flags (consumes argv[i] and its value: true when it was one of them)
bool FCutsceneArg(int argc, char** argv, int& i);
bool FCutsceneToolsWanted();
// game.cpp: after each step's UpdateSw
void UpdateCutsceneTools(SW* psw, int frame);
// the run is over (the trace written): the game loop ends
bool FCutsceneToolsDone();
// --play-scpt's fast-forward to the playhead: steps run without drawing while true
bool FCutsceneSeeking();
// the run's end (game.cpp): the files written
void FinishCutsceneTools(SW* psw);

}  // namespace cut
