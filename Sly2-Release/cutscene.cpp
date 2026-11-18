// cutscene.cpp: the map editor's cutscene editor, game side (cutscene.h): the commands' catalogue and the game-run
// tools (the trace of a script, the play from the editor's playhead, the play session recording).
#include "cutscene.h"
#include "textmod.h"
#include "task.h"
#include "modsave.h"
#include "disc.h"

#include "actor.h"
#include "activate.h"
#include "alo.h"
#include "asega.h"
#include "aseg.h"
#include "blend.h"
#include "brx.h"
#include "editrec.h"
#include "clock.h"
#include "cm.h"
#include "cplcy.h"
#include "joy.h"
#include "jump.h"
#include "lo.h"
#include "po.h"
#include "savedata.h"
#include "scpt.h"
#include "so.h"
#include "sw.h"
#include "worldaccess.h"
#include "worldglobals.h"

#include <nlohmann/json.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

int IdDialogCur();   // sound.cpp
extern int g_frameJoy;   // (joy.cpp: the step's frame)

namespace cut
{

const char* SzEase(int icurve)
{
    switch (icurve)
    {
    case 0: return "linear";
    case 1: return "ease in";
    case 2: return "back (2)";
    case 3: return "ease in / out";
    case 4: return "overshoot (4)";
    case 5: return "snap (5)";
    case ICURVE_EASEOUT: return "ease out";
    default: return "snap (5)";
    }
}
float SEase(int icurve, float u)
{
    static const float s_a[7][3] = {{0, 0, 1}, {-1, 2, 0}, {-1, -1, 1}, {-2, 3, 0}, {5, 0, 0.5f}, {2000, 0, 1}, {0, -1, 2}};
    const float* k = s_a[icurve == ICURVE_EASEOUT ? 6 : std::clamp(icurve, 0, 5)];
    return u * u * u * k[0] + u * u * k[1] + u * k[2];
}

const char* SzTrack(TRACKK tk)
{
    static const char* s_a[] = {"Flow", "Camera", "Characters", "Dialogue", "Sound", "Effects", "Events", "Presentation"};
    return tk >= 0 && tk < TK_Max ? s_a[tk] : "?";
}

namespace
{
// (the "no waiting" word as a field)
constexpr int B = -2;
std::vector<CMDDESC> MakeCatalogue()
{
    std::vector<CMDDESC> a;
    auto add = [&](int kind, const char* name, const char* verb, TRACKK tk, int ibActor, int wait, std::vector<PARDESC> apar, const char* tip) {
        CMDDESC d;
        d.kind = kind, d.name = name, d.verb = verb, d.tk = tk, d.ibActor = ibActor, d.wait = wait, d.apar = std::move(apar), d.tip = tip;
        a.push_back(std::move(d));
    };
    const PARDESC noWait{B, "don't wait", PK_Bool, "on: the next command starts at once; off: the track waits until this one is done"};
    // flow
    add(0x00, "Track", "track", TK_Flow, -1, 0, {{0x10, "object", PK_Object, "the track's own object: its commands act on it when they name none (-1: none)"}},
        "A block of commands run as a track of its own, beside the one it is in (which goes on at once)");
    add(0x01, "End", "end", TK_Flow, -1, 0, {}, "The end of a block");
    add(0x02, "Stop track", "stop-track", TK_Flow, -1, 0, {{0x10, "track", PK_Block, "the track (by its object id) stopped; -1: this one"}}, "Stops a running track");
    add(0x03, "Wait for tracks", "wait-tracks", TK_Flow, -1, 2,
        {{0x10, "count", PK_Int, "how many tracks follow"}, {0x14, "track 1", PK_Block, ""}, {0x18, "track 2", PK_Block, ""}, {0x1C, "track 3", PK_Block, ""}, {0x20, "track 4", PK_Block, ""}},
        "Waits until the named tracks have ended");
    add(0x04, "Wait for alarm", "wait-alarm", TK_Flow, -1, 2, {{0x10, "alarm", PK_Alarm, ""}, {0x14, "clear it", PK_Bool, "the alarm cleared once seen"}},
        "Waits until another track (or another script) sets the alarm");
    add(0x05, "Set alarm", "alarm", TK_Flow, -1, 0, {{0x10, "alarm", PK_Alarm, ""}, {0x14, "script", PK_Script, "the script whose alarm it is; -1: this one"}}, "Sets an alarm (wakes the tracks waiting for it)");
    add(0x06, "Stop script", "stop-script", TK_Events, -1, 0, {{0x10, "script", PK_Script, ""}}, "Stops a running script");
    add(0x07, "Activate", "activate", TK_Events, 0x10, 0, {{0x10, "object", PK_Object, ""}}, "The object's activation (its slot 79: e.g. a switch thrown)");
    add(0x08, "Default object", "default-object", TK_Flow, -1, 0, {{0x10, "object", PK_Object, ""}}, "The object the track's next commands act on when they name none");
    add(0x09, "Script event", "event", TK_Events, -1, 0, {{0x10, "value", PK_Int, "handed to the script's splice handler (its event 4)"}},
        "Calls the script's own splice code (the Scripts tab: its on-event handler) with a value");
    add(0x0A, "Animation", "animate", TK_Actor, 0x14, 1, {{0x10, "animation", PK_Anim, "the animation (ASEG) played"}, {0x14, "on", PK_Actor, "the character / object it plays on (-1: the animation's own objects)"}, {0x18, "flags", PK_Flags, "1 seek, 2 events from the start, 8 loop, 0x10 ping-pong, 0x20 kept at the end"}, noWait},
        "Plays an animation: on a character (its own clip), or a cinematic animation that moves its objects and the camera");
    add(0x0B, "Start script", "start-script", TK_Events, -1, 1, {{0x10, "script", PK_Script, ""}, {0x14, "started by", PK_Object, "the object handed to it (-1: the track's)"}, noWait},
        "Starts another script (waits until it ends unless \"don't wait\")");
    add(0x0C, "Lookup context", "context", TK_Flow, -1, 0, {{0x10, "object", PK_Object, ""}}, "Where the track's ids are looked up from (an instance's objects)");
    add(0x0D, "State machine", "state-machine", TK_Events, 0x10, 1,
        {{0x10, "object", PK_Object, ""}, {0x14, "machine", PK_Object, "-1: any of the object's"}, {0x18, "state", PK_Object, "the state to go to"}, {0x1C, "at once", PK_Bool, "snapped (no transition)"}, noWait},
        "Sends an object's state machine to a state (waits until it is there unless \"don't wait\")");
    add(0x0E, "Show screen", "screen", TK_Present, -1, 1, {{0x10, "screen", PK_Object, "a screen object (cid 0x7E)"}, {0x14, "part", PK_Int, ""}, noWait}, "Shows a screen object (a picture over the game)");
    add(0x0F, "Hide screen", "hide-screen", TK_Present, -1, 0, {{0x10, "screen", PK_Object, ""}}, "Hides a screen object");
    add(0x10, "Time scale", "time-scale", TK_Flow, -1, 0, {{0x10, "scale", PK_Float, "1 normal; 0: the script runs on the real clock (while the game is paused)"}}, "The game's speed while the script runs");
    add(0x11, "Set task state", "task", TK_Events, -1, 0, {{0x10, "task", PK_Task, ""}, {0x14, "state", PK_TaskState, "0 unavailable, 1 available, 2 done, 3 ..."}}, "Sets a task's state (jobs, progress)");
    add(0x12, "Pause game", "pause", TK_Present, -1, 0, {{0x10, "on", PK_Bool, ""}, {0x14, "what", PK_Flags, "the pause's kinds (the pad is always locked; 1: the subtitle box with the letterbox bars, 2: the HUD hidden)"}},
        "Pauses the game around the scene (the pad locked, the letterbox, the HUD hidden): what makes it a cutscene");
    add(0x13, "Label", "label", TK_Flow, -1, 0, {{0x10, "label", PK_Label, ""}}, "A place to jump to");
    add(0x14, "Go to label", "goto", TK_Flow, -1, 0, {{0x10, "label", PK_Label, ""}}, "Jumps to a label of this track (a loop)");
    add(0x15, "Go to label if", "goto-if", TK_Flow, -1, 0, {{0x10, "label", PK_Label, ""}, {0x14, "unless", PK_Int, "jumps only when 0"}}, "Jumps to a label");
    add(0x16, "Place", "place", TK_Actor, 0x18, 0, {{0x10, "at", PK_Object, "the object whose position and heading it takes (a warp: the player warped)"}, {0x14, "keep its state", PK_Bool, "off: a character stands still there"}, {0x18, "object", PK_Actor, ""}},
        "Puts a character / object at another object's place and heading");
    add(0x17, "Camera cut", "cut", TK_Camera, -1, 0, {}, "The camera cuts (no blend to its next view)");
    add(0x1A, "Climb pipe to", "climb-to", TK_Actor, 0x18, 1, {{0x10, "to", PK_Object, ""}, {0x14, "within", PK_Distance, ""}, {0x18, "actor", PK_Actor, ""}, noWait}, "A climbing character climbs its pipe to the point nearest an object");
    add(0x1D, "Reach", "reach", TK_Actor, 0x14, 2, {{0x10, "object", PK_Object, ""}, {0x14, "actor", PK_Actor, ""}}, "The character reaches for an object (its reach animation)");
    add(0x1E, "Hand over", "hand", TK_Actor, 0x18, 2, {{0x10, "object", PK_Object, "the object thrown / handed"}, {0x14, "to", PK_Object, ""}, {0x18, "actor", PK_Actor, ""}}, "The character tosses an object to another");
    add(0x1F, "Interact", "interact", TK_Actor, 0x14, 2, {{0x10, "object", PK_Object, ""}, {0x14, "actor", PK_Actor, ""}}, "The character's interaction with an object");
    add(0x20, "Grab / let go", "grab", TK_Actor, 0x14, 0, {{0x10, "object", PK_Object, "-1: lets go of what it holds"}, {0x14, "actor", PK_Actor, ""}}, "The character takes hold of an object, or lets go");
    add(0x21, "Put down", "put", TK_Actor, 0x14, 2, {{0x10, "where", PK_Object, ""}, {0x14, "actor", PK_Actor, ""}}, "The character puts what it holds at an object");
    add(0x22, "Prize animation", "prize", TK_Actor, 0x1C, 2, {{0x10, "animation", PK_Anim, ""}, {0x14, "facing", PK_Object, "3: the camera"}, {0x18, "value", PK_Int, ""}, {0x1C, "actor", PK_Actor, ""}}, "The character's prize / pose animation, facing an object");
    add(0x23, "Face", "face", TK_Actor, 0x18, 2, {{0x10, "seconds", PK_Seconds, "how long it holds"}, {0x14, "toward", PK_Object, ""}, {0x18, "actor", PK_Actor, ""}}, "The character turns toward an object and holds for a time");
    add(0x24, "Go to", "go", TK_Actor, 0x1C, 1,
        {{0x10, "to", PK_Object, ""}, {0x14, "stick", PK_Stick, "how far the stick is pushed (0..1: walk to run)"}, {0x18, "within", PK_Distance, "done this near"}, {0x1C, "actor", PK_Actor, ""}, {0x20, "animation", PK_Anim, "-1: its own"}, {0x24, "walk button", PK_Bool, ""}, noWait},
        "The character is steered to an object as the player would (the stick)");
    add(0x25, "Avoid off", "run-on", TK_Actor, -1, 0, {}, "The track's next walks run (and follow paths without avoiding others)");
    add(0x26, "Avoid on", "run-off", TK_Actor, -1, 0, {}, "The track's next walks walk again");
    add(0x27, "Follow path", "follow", TK_Actor, 0x20, 2,
        {{0x10, "path", PK_Path, "a path object (a curve)"}, {0x14, "from its start", PK_Bool, "on: along the path from its first point (as the game's scenes do)"}, {0x18, "walk", PK_Bool, "on: walks; off: runs"}, {0x1C, "speed", PK_Speed, "how far its stick is pushed (1: full, its own walk / run speed; 0: it stands still)"}, {0x20, "actor", PK_Actor, ""}, {0x24, "animation", PK_Anim, "-1: its own walk / run"}},
        "The character walks / runs along a path (waits until it arrives)");
    add(0x28, "Walk to", "walk", TK_Actor, 0x20, 2,
        {{0x10, "to", PK_Object, "the object (a point, a marker ...) it goes to"}, {0x14, "arrive within", PK_Distance, ""}, {0x18, "walk", PK_Bool, "on: walks; off: runs"}, {0x1C, "speed", PK_Speed, "how far its stick is pushed (1: full, its own walk / run speed; 0: it stands still)"}, {0x20, "actor", PK_Actor, ""}, {0x24, "animation", PK_Anim, "-1: its own walk / run"}},
        "The character walks / runs to an object (waits until it arrives)");
    add(0x29, "Hiding rail to", "rail-to", TK_Actor, 0x1C, 1, {{0x10, "to", PK_Object, ""}, {0x14, "stick", PK_Stick, ""}, {0x18, "within", PK_Distance, ""}, {0x1C, "actor", PK_Actor, ""}, noWait}, "A character on a hiding rail moves along it");
    add(0x2A, "Jump to", "jump", TK_Actor, 0x1C, 1, {{0x10, "to", PK_Object, "-1: where it stands"}, {0x14, "camera", PK_Int, "1: the camera follows the jump"}, {0x18, "kind", PK_Int, ""}, {0x1C, "actor", PK_Actor, ""}, noWait}, "The character jumps to an object (a ledge, a point, the ground)");
    add(0x2B, "Launch to", "launch", TK_Actor, 0x1C, 0, {{0x10, "to", PK_Object, ""}, {0x14, "height", PK_Float, "the arc's share of height"}, {0x18, "air control", PK_Float, ""}, {0x1C, "actor", PK_Actor, ""}, noWait}, "The character is thrown toward an object");
    add(0x2C, "Show", "show", TK_Effects, 0x10, 1, {{0x10, "object", PK_Object, ""}, {0x14, "fade", PK_Seconds, "the fade in"}, noWait}, "Puts an object into the world (faded in)");
    add(0x2D, "Hide", "hide", TK_Effects, 0x10, 1, {{0x10, "object", PK_Object, ""}, {0x14, "fade", PK_Seconds, "the fade out"}, noWait}, "Takes an object out of the world (faded out)");
    add(0x2E, "Show parts", "show-parts", TK_Effects, 0x10, 0, {{0x10, "object", PK_Object, ""}, {0x14, "tag", PK_Int, "the model parts of this tag"}}, "Shows parts of an object's model");
    add(0x2F, "Hide parts", "hide-parts", TK_Effects, 0x10, 0, {{0x10, "object", PK_Object, ""}, {0x14, "tag", PK_Int, ""}}, "Hides parts of an object's model");
    add(0x30, "Collision on", "collide-on", TK_Effects, 0x10, 0, {{0x10, "object", PK_Object, ""}, {0x14, "tag", PK_Int, ""}}, "Turns parts of an object's collision on");
    add(0x31, "Collision off", "collide-off", TK_Effects, 0x10, 0, {{0x10, "object", PK_Object, ""}, {0x14, "tag", PK_Int, ""}}, "Turns parts of an object's collision off");
    add(0x32, "Camera shake", "shake", TK_Camera, -1, 1, {{0x10, "kind", PK_Int, ""}, noWait}, "Shakes the camera (half a second)");
    add(0x33, "Rumble", "rumble", TK_Effects, -1, 1, {{0x10, "seconds", PK_Seconds, ""}, {0x14, "kind", PK_Int, ""}, noWait}, "The pad's rumble");
    add(0x34, "Explode", "explode", TK_Effects, 0x10, 0, {{0x10, "object", PK_Object, ""}, {0x14, "source", PK_Object, ""}, {0x18, "touching", PK_Object, ""}, {0x1C, "scale", PK_Float, ""}}, "An object bursts (its explosion effect)");
    add(0x35, "Eyes", "eyes", TK_Actor, -1, 0, {{0x10, "object", PK_Object, ""}, {0x14, "closed", PK_Int, ""}, {0x18, "time", PK_Float, ""}}, "A character's eyes closed / opened");
    add(0x36, "Sound", "sound", TK_Sound, 0x14, 0,
        {{0x10, "sound", PK_Sound, ""}, {0x14, "at", PK_Object, "-1: not placed"}, {0x18, "full within", PK_Distance, ""}, {0x1C, "silent beyond", PK_Distance, ""}, {0x20, "pitch", PK_Float, ""}, {0x24, "fade", PK_Float, ""}, {0x28, "volume", PK_Float, ""}},
        "Plays a sound effect");
    add(0x37, "Stop sound", "stop-sound", TK_Sound, -1, 0, {{0x10, "sound", PK_Sound, "-1: the track's looping sound"}}, "Stops the track's looping sound");
    add(0x38, "Camera move", "camera", TK_Camera, -1, 0,
        {{0x10, "from", PK_Pos, "the camera's position at the start (0 0 0: where it is)"}, {0x20, "angles", PK_Angles, "pan / tilt / roll in degrees (0 0 0: as it is)"}, {0x30, "fov", PK_Fov, "the field of view (radians; 0: as it is)"}, {0x40, "seconds", PK_Seconds, "the move's length (0: the commands' up to its end)"}, {0x44, "easing", PK_Ease, "how the move starts and ends: 0 linear, 1 ease in, 3 ease in / out, 1000 ease out (SafeHouse's)"}},
        "A camera shot: the camera moves from this view to the one at its end (\"Camera move end\") over its seconds");
    add(0x39, "Camera move end", "camera-end", TK_Camera, -1, 2,
        {{0x10, "to", PK_Pos, ""}, {0x20, "angles", PK_Angles, ""}, {0x30, "fov", PK_Fov, ""}, {0x40, "release", PK_Bool, "the camera given back at the end"}, {0x44, "cut back", PK_Bool, "cut (not blended) back"}},
        "The camera shot's end view (waits until the shot's time is over)");
    add(0x3A, "Camera release", "camera-release", TK_Camera, -1, 0, {{0x10, "cut", PK_Bool, ""}}, "Gives the camera back to the game");
    add(0x3B, "Binocucom", "binoc", TK_Dialog, 0x10, 1, {{0x10, "actor", PK_Actor, ""}, {0x14, "speaker", PK_Object, "the other speaker"}, {0x18, "window", PK_Object, "its screen object"}, noWait}, "Opens the binocucom talk window");
    add(0x3C, "Binocucom close", "binoc-close", TK_Dialog, -1, 1, {noWait}, "Closes the binocucom");
    add(0x3D, "Camera focus", "focus", TK_Camera, -1, 0, {{0x10, "object", PK_Object, ""}}, "The free camera looks at an object");
    add(0x3E, "Camera follows", "follow-camera", TK_Camera, -1, 0, {{0x10, "object", PK_Object, ""}}, "The free camera follows an object");
    add(0x3F, "Free camera", "free-camera", TK_Camera, -1, 0, {{0x10, "position", PK_Pos, ""}, {0x20, "angles", PK_Angles, ""}, {0x30, "distance", PK_Float, ""}}, "The free camera's offsets");
    add(0x40, "Camera zoom", "zoom", TK_Camera, -1, 0, {{0x10, "zoom", PK_Float, ""}, noWait}, "The free camera's zoom");
    add(0x41, "Camera snap", "snap", TK_Camera, -1, 0, {}, "The free camera jumps to its target");
    add(0x42, "Wait for button", "wait-button", TK_Flow, -1, 2, {{0x10, "button", PK_Button, ""}, {0x14, "player", PK_Object, ""}}, "Waits for a button press");
    add(0x43, "Wait for character", "wait-actor", TK_Actor, 0x14, 2, {{0x10, "state", PK_Int, "its jump kind (0: standing)"}, {0x14, "actor", PK_Actor, ""}}, "Waits until a character is in a state (e.g. standing)");
    add(0x44, "Title card", "title", TK_Present, -1, 2, {}, "Shows the title card (waits until it is gone)");
    add(0x45, "Line", "line", TK_Dialog, -1, 1, {{0x10, "line", PK_Line, "the dialogue line (voice, subtitle, speaker)"}, {0x14, "kind", PK_Int, ""}, noWait}, "A line of dialogue (its voice and subtitle)");
    add(0x46, "Clear subtitles", "clear-subtitles", TK_Dialog, -1, 0, {}, "Takes the subtitle away");
    add(0x47, "Hold face", "hold-face", TK_Actor, 0x10, 0, {{0x10, "object", PK_Actor, ""}, {0x14, "what", PK_Flags, ""}, {0x18, "value", PK_Int, ""}}, "A character's talking face held");
    add(0x48, "Release face", "release-face", TK_Actor, 0x10, 0, {{0x10, "object", PK_Actor, ""}, {0x14, "what", PK_Flags, ""}, {0x18, "value", PK_Int, ""}}, "A held face let go");
    add(0x49, "Dance", "dance", TK_Events, -1, 0, {{0x10, "state", PK_Int, ""}}, "The dance game's state (its beats follow)");
    add(0x4A, "Dance beat", "beat", TK_Events, 0x1C, 2, {{0x10, "music time", PK_Seconds, ""}, {0x14, "button", PK_Button, ""}, {0x18, "move", PK_Int, ""}, {0x1C, "dancer", PK_Actor, ""}}, "A beat of the dance game");
    add(0x4B, "Dance move", "dance-move", TK_Actor, 0x14, 0, {{0x10, "animation", PK_Anim, ""}, {0x14, "dancer", PK_Actor, ""}}, "A dancer's move");
    add(0x4C, "Music", "music", TK_Sound, -1, 0, {{0x10, "music", PK_Music, ""}}, "Starts a music cue");
    add(0x4D, "Music off", "music-off", TK_Sound, -1, 0, {{0x10, "music", PK_Music, ""}}, "Stops a music cue");
    add(0x4E, "Wait for music", "wait-music", TK_Sound, -1, 2, {{0x10, "music time", PK_Seconds, ""}}, "Waits until the music reaches a time");
    add(0x4F, "Murray holds", "murray-hold", TK_Actor, 0x10, 1, {{0x10, "Murray", PK_Actor, ""}, {0x14, "object", PK_Object, ""}, {0x18, "value", PK_Int, ""}, {0x1C, "state", PK_Object, ""}, {0x20, "value 2", PK_Int, ""}, noWait}, "Murray holds an object up (a gate)");
    add(0x50, "Bentley's ability", "bentley-on", TK_Actor, 0x10, 2, {{0x10, "Bentley", PK_Actor, ""}}, "Bentley's ability held (until he uses it)");
    add(0x51, "Bentley's ability off", "bentley-off", TK_Actor, 0x10, 2, {{0x10, "Bentley", PK_Actor, ""}}, "Bentley's ability let go");
    add(0x52, "Wait for line", "wait-line", TK_Dialog, -1, 2, {{0x10, "at most", PK_Seconds, "-1: to the line's end"}}, "Waits for the track's dialogue line to end (or for a time into it)");
    add(0x53, "Hint", "hint", TK_Present, -1, 1, {{0x10, "text", PK_Text, ""}, {0x14, "seconds", PK_Seconds, ""}, noWait}, "A hint box on the screen");
    add(0x54, "Hint off", "hint-off", TK_Present, -1, 0, {{0x10, "text", PK_Text, ""}}, "Takes a hint box away");
    add(0x55, "Guard commands", "guard", TK_Actor, 0x10, 1, {{0x10, "guard", PK_Actor, ""}, {0x14, "commands", PK_Object, "a command list"}, noWait}, "A guard runs a command list");
    add(0x56, "Guard commands off", "guard-off", TK_Actor, 0x10, 0, {{0x10, "guard", PK_Actor, ""}, {0x14, "commands", PK_Object, ""}}, "A guard stops a command list");
    add(0x57, "Murray's toss", "murray-toss", TK_Actor, 0x10, 0, {{0x10, "Murray", PK_Actor, ""}}, "Murray's Bentley toss");
    add(0x58, "Partner jump", "partner-jump", TK_Actor, 0x14, 1, {{0x10, "Murray", PK_Object, ""}, {0x14, "Bentley", PK_Actor, ""}, noWait}, "Bentley jumps into Murray's hands");
    add(0x59, "Murray aims", "murray-aim", TK_Actor, 0x14, 1, {{0x10, "at", PK_Object, ""}, {0x14, "Murray", PK_Actor, ""}, noWait}, "Murray aims his toss");
    add(0x5A, "Murray throws", "murray-throw", TK_Actor, 0x14, 1, {{0x10, "at", PK_Object, ""}, {0x14, "Murray", PK_Actor, ""}, noWait}, "Murray throws");
    return a;
}
}  // namespace

const std::vector<CMDDESC>& AllCommands()
{
    static const std::vector<CMDDESC> s_a = MakeCatalogue();
    return s_a;
}

const CMDDESC& DescOfKind(int kind)
{
    static const std::vector<CMDDESC> s_aUnknown = [] {
        std::vector<CMDDESC> a(0x5B);
        for (int k = 0; k < 0x5B; ++k)
        {
            a[size_t(k)].kind = k;
            a[size_t(k)].name = "Command (not known)";
            a[size_t(k)].verb = "raw";
            a[size_t(k)].tk = TK_Events;
        }
        return a;
    }();
    static const std::vector<int> s_ai = [] {
        std::vector<int> a(0x5B, -1);
        const auto& acmd = AllCommands();
        for (size_t i = 0; i < acmd.size(); ++i) a[size_t(acmd[i].kind)] = int(i);
        return a;
    }();
    static const CMDDESC s_none;
    if (kind < 0 || kind >= 0x5B) return s_none;
    return s_ai[size_t(kind)] >= 0 ? AllCommands()[size_t(s_ai[size_t(kind)])] : s_aUnknown[size_t(kind)];
}

int IbObjectOfKind(int kind)
{
    switch (kind)
    {
    case 0x16: case 0x1A: case 0x1C: case 0x1E: case 0x23: return 0x18;
    case 0x27: case 0x28: return 0x20;
    case 0x22: case 0x24: case 0x29: case 0x2A: case 0x2B: case 0x4A: return 0x1C;
    case 0x1B: case 0x47: case 0x48: case 0x4F: case 0x50: case 0x51: case 0x57: return 0x10;
    case 0x0B: case 0x18: case 0x19: case 0x1D: case 0x1F: case 0x20: case 0x21: case 0x43: case 0x4B: case 0x58: case 0x59: case 0x5A: return 0x14;
    default: return -1;
    }
}

const std::vector<BUILDER>& Builders()
{
    static const std::vector<BUILDER> s_a = [] {
        std::vector<BUILDER> a;
        for (int eopid = 0; eopid < 4096; ++eopid)
        {
            BUILDER b;
            if (!FEditorOptionMethod(eopid, b.pfn, b.cArgs) || !b.pfn) continue;
            if (!FScptBuilderLayout(b.pfn, b.kind, b.alay)) continue;
            b.eopid = eopid;
            a.push_back(std::move(b));
        }
        return a;
    }();
    return s_a;
}
const BUILDER* PbuilderOfEopid(int eopid)
{
    for (const BUILDER& b : Builders())
        if (b.eopid == eopid) return &b;
    return nullptr;
}
const BUILDER* PbuilderOfKind(int kind, int variant)
{
    for (const BUILDER& b : Builders())
        if (b.kind == kind && variant-- == 0) return &b;
    return nullptr;
}

// ---- the game-run tools

namespace
{
struct TRACEOBJ
{
    int oid = 0, cid = 0;
    glm::vec3 posLast{0.0f};
    glm::quat qLast{1, 0, 0, 0};
    bool fInLast = true, fKeyed = false;
    nlohmann::json akey = nlohmann::json::array();
};
struct TOOLS
{
    // --cutscene-trace
    int oidTrace = 0;
    int frameStart = 60;
    std::string out;
    bool fStarted = false, fDone = false, fWritten = false;
    LO* pscpt = nullptr;
    float t0 = 0.0f, tEnd = -1.0f;
    int frameStopped = -1, frameFound = 0;
    SW* pswStart = nullptr;
    bool fLeft = false, fTimedOut = false, fFocusStarted = false;
    int oidFocus = 0;
    float tFocus = 0.0f;
    float tMax = 240.0f;
    std::set<const void*> setAmbient;
    std::unordered_map<const void*, int> mprun;
    nlohmann::json aruns = nlohmann::json::array(), acmds = nlohmann::json::array(), acam = nlohmann::json::array(), alines = nlohmann::json::array(),
                   aanims = nlohmann::json::array();
    std::map<int, TRACEOBJ> mpobj;   // (by the object's index in aploAll: a clone's oid repeats)
    std::map<std::pair<const void*, const void*>, size_t> mpanim;   // the ASEGAs seen: (asega, aseg) -> aanims index
    struct ANIMKEY { float t = 0, at = 0, rate = 1; int id = -1; };
    std::map<size_t, ANIMKEY> mpanimLast;                            // an animation's last clip-time key
    int idLine = -1;
    float tLine = 0.0f;
    // --play-scpt
    int oidPlay = 0;
    float tSeek = 0.0f;
    bool fPlayStarted = false, fSeeking = false;
    // --record-session
    std::string record;
    nlohmann::json arec = nlohmann::json::array();
    int frameRec0 = -1;
};
TOOLS& Tools()
{
    static TOOLS s;
    return s;
}
float TNow() { return g_clock.t3C; }
float Heading(LO* plo);
// (an object's own record: an actor's moves under its root record)
const XF* PxfMoved(LO* plo)
{
    if (!FIsLoDerivedFrom(plo, 0x30)) return nullptr;
    ALO* palo = static_cast<ALO*>(plo);
    if (const XF* pxf = PxfObj(palo)) return pxf;
    return palo->axf.empty() ? nullptr : palo->axf[0].get();
}
glm::quat Rot(LO* plo)
{
    const XF* pxf = PxfMoved(plo);
    if (!pxf) return glm::quat(1, 0, 0, 0);
    glm::mat3 r(pxf->matWorld);
    for (int i = 0; i < 3; ++i)
        if (const float l = glm::length(r[i]); l > 1e-6f) r[i] /= l;
    return glm::normalize(glm::quat_cast(r));
}
glm::vec3 Pos(LO* plo) { return PosLoWorld(plo); }
float Heading(LO* plo)
{
    const XF* pxf = PxfMoved(plo);
    return pxf ? std::atan2(pxf->matWorld[0].y, pxf->matWorld[0].x) : 0.0f;
}
float R2(float g) { return std::round(g * 100.0f) / 100.0f; }
float R4(float g) { return std::round(g * 10000.0f) / 10000.0f; }

void OnScptTrace(LO* pscpt, const void* prun, int ev, int ic, const void* prunParent)
{
    TOOLS& t = Tools();
    if (!t.fStarted || t.fDone || !pscpt || pscpt->oid != (t.oidFocus ? t.oidFocus : t.oidTrace)) return;
    if (!t.fFocusStarted)
    {
        // (the scene's own start: the trace's time 0; what its starter did before has negative times)
        t.fFocusStarted = true;
        t.tFocus = TNow() - t.t0;
        // (what was recorded before: its times from the scene's start too)
        const float d = t.tFocus;
        for (auto& c : t.acam) c[0] = R4(c[0].get<float>() - d);
        for (auto& [i, o] : t.mpobj)
            for (auto& k : o.akey) k[0] = R4(k[0].get<float>() - d);
        for (auto* pa : {&t.alines, &t.aanims})
            for (auto& l : *pa) { l["t0"] = R4(l["t0"].get<float>() - d); l["t1"] = R4(l["t1"].get<float>() - d); }
        for (auto& a : t.aanims)
            for (auto& k : a["keys"]) k[0] = R4(k[0].get<float>() - d);
        for (auto& [i, l] : t.mpanimLast) l.t -= d;
        t.tLine -= d;
    }
    const float tt = R4(TNow() - t.t0 - t.tFocus);
    if (ev == 0)
    {
        const int id = int(t.aruns.size());
        t.mprun[prun] = id;
        const auto it = t.mprun.find(prunParent);
        t.aruns.push_back({{"run", id}, {"block", ic}, {"parent", it == t.mprun.end() ? -1 : it->second}, {"t0", tt}, {"t1", -1}});
    }
    else
    {
        const auto it = t.mprun.find(prun);
        if (it == t.mprun.end()) return;
        if (ev == 1) t.acmds.push_back({it->second, ic, tt});
        else
        {
            t.aruns[size_t(it->second)]["t1"] = tt;
            t.mprun.erase(it);   // (its memory may be a later runner's)
        }
    }
}

void TraceStep(SW* psw)
{
    TOOLS& t = Tools();
    const float tt = R4(TNow() - t.t0 - t.tFocus);
    // the camera: position, its rows (x the view, y left, z up), the field of view
    {
        nlohmann::json c = {tt, R2(g_cm.pos.x), R2(g_cm.pos.y), R2(g_cm.pos.z)};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) c.push_back(R4(g_cm.mat[i][j]));
        c.push_back(R4(g_cm.radFOV));
        t.acam.push_back(std::move(c));
    }
    // the objects that moved or were shown / hidden (root objects and characters)
    for (size_t i = 0; i < psw->aploAll.size(); ++i)
    {
        LO* plo = psw->aploAll[i].get();
        if (!FIsLoDerivedFrom(plo, 0x30) || (plo->paloParent && !FIsLoDerivedFrom(plo, 0x34))) continue;
        const glm::vec3 p = Pos(plo);
        const glm::quat q = Rot(plo);
        const bool fIn = plo->fInWorld;
        auto key = [&]() { return nlohmann::json{tt, R2(p.x), R2(p.y), R2(p.z), R4(q.x), R4(q.y), R4(q.z), R4(q.w), fIn ? 1 : 0}; };
        auto [it, fNew] = t.mpobj.try_emplace(int(i));
        TRACEOBJ& o = it->second;
        if (fNew)
        {
            o.oid = plo->oid, o.cid = int(plo->Cid());
            o.posLast = p, o.qLast = q, o.fInLast = fIn;
            o.akey.push_back(key());
            continue;
        }
        if (glm::length(p - o.posLast) > 0.5f || std::fabs(glm::dot(q, o.qLast)) < 0.99995f || fIn != o.fInLast)
        {
            o.posLast = p, o.qLast = q, o.fInLast = fIn;
            o.fKeyed = true;
            o.akey.push_back(key());
        }
    }
    // the animations playing (their spans; the ones playing before the scene "ambient": its characters' idles), with
    // keys [t, clip time, rate, clip] where the clip time stops running on as the last key predicts (a speed, a pause,
    // a loop, a seek). A blend (a gait: walk / run, cid 0x6E) is keyed by its heaviest clip at its share of the blend's
    // time. The editor poses the characters at the playhead from them.
    for (ASEGA* pa : g_apasega)
    {
        if (!pa || !pa->paseg) continue;
        const bool fAmbient = t.setAmbient.count(pa) != 0;
        if (fAmbient && !pa->palo) continue;   // (the world's own loops: not a character's)
        int idClip = pa->paseg->oid;
        float tcl = pa->t;
        if (FIsLoDerivedFrom(pa->paseg, 0x6E))
        {
            const ASEGBL* pbl = static_cast<const ASEGBL*>(pa->paseg);
            const BLENDBL* pbest = nullptr;
            for (const BLENDBL& bl : pbl->abl)
                if (bl.paseg && (!pbest || bl.weight > pbest->weight)) pbest = &bl;
            if (pbest && PasegdOf(pbest->paseg) && pbl->TMax() > 1e-4f)
            {
                idClip = pbest->paseg->oid;
                tcl = std::fmod(std::max(0.0f, pa->t), pbl->TMax()) / pbl->TMax() * PasegdOf(pbest->paseg)->tMax;
            }
        }
        const auto key = std::make_pair(static_cast<const void*>(pa), static_cast<const void*>(pa->paseg));
        auto it = t.mpanim.find(key);
        if (it == t.mpanim.end())
        {
            const size_t i = t.aanims.size();
            t.mpanim[key] = i;
            t.aanims.push_back({{"aseg", pa->paseg->oid}, {"on", pa->palo ? pa->palo->oid : -1}, {"t0", tt}, {"t1", tt}, {"at", R2(pa->t)},
                                {"length", R2(PasegdOf(pa->paseg) ? PasegdOf(pa->paseg)->tMax : 0.0f)},
                                {"keys", nlohmann::json::array({nlohmann::json::array({tt, R4(tcl), 1.0f, idClip})})}});
            if (fAmbient) t.aanims[i]["ambient"] = 1;
            t.mpanimLast[i] = {tt, tcl, 1.0f, idClip};
        }
        else if (!pa->fRetracted)
        {
            auto& a = t.aanims[it->second];
            a["t1"] = tt;
            auto& last = t.mpanimLast[it->second];
            if (idClip != last.id || std::fabs(tcl - (last.at + last.rate * (tt - last.t))) > 0.004f)
            {
                const float rate = idClip == last.id && tt - last.t > 1e-4f ? (tcl - last.at) / (tt - last.t) : 1.0f;
                const float r = std::fabs(rate) < 0.02f ? 0.0f : R4(rate);
                a["keys"].push_back({tt, R4(tcl), r, idClip});
                last = {tt, tcl, r, idClip};
            }
        }
    }
    // the dialogue line
    const int id = IdDialogCur();
    if (id != t.idLine)
    {
        if (t.idLine != -1) t.alines.push_back({{"line", t.idLine}, {"t0", t.tLine}, {"t1", tt}});
        t.idLine = id;
        t.tLine = tt;
    }
}

void WriteTrace(SW* psw)
{
    TOOLS& t = Tools();
    if (t.fWritten || t.out.empty()) return;
    t.fWritten = true;
    if (t.idLine != -1) t.alines.push_back({{"line", t.idLine}, {"t0", t.tLine}, {"t1", R4(TNow() - t.t0 - t.tFocus)}});
    nlohmann::json j;
    j["format"] = "sly2-cutscene-trace";
    j["version"] = 1;
    j["keys"] = "t x y z qx qy qz qw in";
    j["script"] = t.oidTrace;
    j["started"] = t.fStarted;
    j["end"] = (t.tEnd >= 0.0f ? t.tEnd : R4(TNow() - t.t0)) - R4(t.tFocus);
    j["focus"] = t.oidFocus ? t.oidFocus : t.oidTrace;
    j["focus_started"] = t.fFocusStarted;
    j["stopped"] = t.tEnd >= 0.0f && !t.fTimedOut;
    j["left_world"] = t.fLeft;
    j["timed_out"] = t.fTimedOut;
    j["runs"] = t.aruns;
    j["cmds"] = t.acmds;
    j["camera"] = t.acam;
    j["lines"] = t.alines;
    j["anims"] = t.aanims;
    nlohmann::json ao = nlohmann::json::array();
    for (auto& [i, o] : t.mpobj)
        if (o.fKeyed) ao.push_back({{"oid", o.oid}, {"cid", o.cid}, {"keys", o.akey}});
    j["objects"] = ao;
    if (psw)
        if (LO* pp = PloPoCurrent(psw)) j["player"] = pp->oid;
    {
        // (written aside and renamed: the editor polls for the file and must not read it half written)
        std::ofstream f(t.out + ".part", std::ios::binary);
        f << j.dump();
    }
    std::remove(t.out.c_str());
    std::rename((t.out + ".part").c_str(), t.out.c_str());
    std::printf("cutscene: trace of %d written to %s (%.2f s, %zu commands, %zu objects moved)\n", t.oidTrace, t.out.c_str(), double(j["end"].get<float>()),
                t.acmds.size(), ao.size());
}

void RecordStep(SW* psw, int frame)
{
    TOOLS& t = Tools();
    LO* pp = PloPoCurrent(psw);
    if (!pp) return;
    if (t.frameRec0 < 0) t.frameRec0 = frame;
    const glm::vec3 p = Pos(pp);
    ACTOR* pa = PactorFromLo(pp);
    nlohmann::json r = {R4(float(frame - t.frameRec0) / 60.0f), pp->oid, R2(p.x), R2(p.y), R2(p.z), R4(Heading(pp))};
    glm::vec3 v(0.0f);
    if (FIsLoDerivedFrom(pp, 0x30)) v = VelAloPos(static_cast<ALO*>(pp));
    r.push_back(R2(std::sqrt(v.x * v.x + v.y * v.y)));
    r.push_back(R2(v.z));
    r.push_back(pa ? ActorJumpKind(pa) : -1);
    // the clip playing on it (the newest animation on the player)
    int oidAnim = -1;
    for (ASEGA* q : g_apasega)
        if (q && q->paseg && !q->fRetracted && q->palo == pp) oidAnim = q->paseg->oid;
    r.push_back(oidAnim);
    r.push_back(R2(g_joyps2.x));
    r.push_back(R2(g_joyps2.y));
    r.push_back(int(g_joyps2.grfbtn));
    t.arec.push_back(std::move(r));
}
}  // namespace

// ---- the cutscene rules

namespace
{
int s_frameSkipStop = -100;
const cane::mods::CutsceneRule* PruleOf(LO* pscpt, std::string* pid = nullptr, std::string* pworld = nullptr)
{
    if (!pscpt || !g_disc.FCutsceneRules()) return nullptr;
    const std::string world = g_disc.SzWorldName(g_isectorWorldLoading);
    if (pworld) *pworld = world;
    return world.empty() ? nullptr : g_disc.PcutsceneRule(world, pscpt->oid, pid);
}
}  // namespace

bool FCutsceneMayStart(LO* pscpt)
{
    std::string id, world;
    const cane::mods::CutsceneRule* pr = PruleOf(pscpt, &id, &world);
    static const bool s_fDbg = std::getenv("SLY2N_CUTRULE") != nullptr;   // (debug: each start and its rule)
    if (s_fDbg) std::printf("cutscene: start %d in %s: rule %s\n", pscpt ? pscpt->oid : -1, world.c_str(), pr ? id.c_str() : "none");
    if (!pr) return true;
    // (the editor's measurements and plays start the scene as it is)
    if (Tools().oidTrace || Tools().oidPlay) return true;
    if (pr->task >= 0)
    {
        bool fDone = false;
        const int n = NTaskStateOid(pr->task, &fDone);
        if (std::find(pr->states.begin(), pr->states.end(), n) == pr->states.end())
        {
            std::printf("cutscene: script %d not started (task %d is %d)\n", pscpt->oid, pr->task, n);
            return false;
        }
    }
    if (pr->once)
    {
        if (FModSceneSeen(id, world, pscpt->oid))
        {
            std::printf("cutscene: script %d not started (seen before)\n", pscpt->oid);
            return false;
        }
        SetModSceneSeen(id, world, pscpt->oid);
    }
    return true;
}

int SkipKindByRule(LO* pscpt, int k)
{
    const cane::mods::CutsceneRule* pr = PruleOf(pscpt);
    if (!pr || pr->skip == "auto") return k;
    if (pr->skip == "never") return 0;
    return k ? k : 2;   // (always: a cinematic when the game's own rule finds none)
}

bool FCutsceneNeverSkip(LO* pscpt)
{
    const cane::mods::CutsceneRule* pr = PruleOf(pscpt);
    return pr && pr->skip == "never";
}

void NoteScptLabel(LO* pscpt, int n)
{
    const cane::mods::CutsceneRule* pr = PruleOf(pscpt);
    if (std::getenv("SLY2N_CUTRULE")) std::printf("cutscene: script %d at label %d (rule %d)\n", pscpt ? pscpt->oid : -1, n, pr ? pr->skipToLabel : -2);
    if (pr && pr->skipToLabel >= 0 && pr->skipToLabel == n) s_frameSkipStop = g_frameJoy;
}

bool FCutsceneSkipStops() { return g_frameJoy - s_frameSkipStop <= 2; }

bool FCutsceneArg(int argc, char** argv, int& i)
{
    TOOLS& t = Tools();
    const char* a = argv[i];
    if (i + 1 >= argc) return false;
    if (!std::strcmp(a, "--cutscene-trace"))
    {
        t.oidTrace = std::atoi(argv[++i]);
        g_fSavesReadOnly = true;   // (a trace never writes the saves)
        // (the lines end after their length in step time (sound.cpp): the trace runs faster than real time and its
        // times are the lines' own, not the audio thread's)
#ifdef _WIN32
        _putenv_s("SLY2N_FASTDIALOG", "1");
#else
        setenv("SLY2N_FASTDIALOG", "1", 1);
#endif
        return true;
    }
    if (!std::strcmp(a, "--cutscene-focus")) { t.oidFocus = std::atoi(argv[++i]); return true; }
    if (!std::strcmp(a, "--cutscene-out"))
    {
        t.out = argv[++i];
        // (a trace's own messages into OUT.log, not the editor's console that started it)
        if (t.oidTrace && std::getenv("SLY2N_CUTLOG") == nullptr)
        {
            const std::string log = t.out + ".log";
            if (std::freopen(log.c_str(), "w", stdout)) std::freopen(log.c_str(), "a", stderr);
        }
        return true;
    }
    if (!std::strcmp(a, "--cutscene-max")) { t.tMax = float(std::atof(argv[++i])); return true; }
    if (!std::strcmp(a, "--cutscene-at")) { t.frameStart = std::max(1, std::atoi(argv[++i])); return true; }
    if (!std::strcmp(a, "--play-scpt"))
    {
        const std::string s = argv[++i];
        t.oidPlay = std::atoi(s.c_str());
        if (const auto at = s.find('@'); at != std::string::npos) t.tSeek = float(std::atof(s.c_str() + at + 1));
        return true;
    }
    if (!std::strcmp(a, "--record-session")) { t.record = argv[++i]; return true; }
    return false;
}

bool FCutsceneToolsWanted()
{
    const TOOLS& t = Tools();
    return t.oidTrace || t.oidPlay || !t.record.empty();
}

static LO* PscptFind(SW* psw, int oid)
{
    if (static bool s_fList = std::getenv("SLY2N_CUTLIST") != nullptr; s_fList)   // (debug: the world's scripts with lines / camera shots)
    {
        s_fList = false;
        for (const auto& up : psw->aploAll)
        {
            if (!FIsLoDerivedFrom(up.get(), 0x72)) continue;
            int cLine = 0, cCam = 0, cPause = 0, kind, b;
            int32_t an[18];
            for (int i = 0; FScptCommand(up.get(), i, kind, b, an); ++i) cLine += kind == 0x45, cCam += kind == 0x38, cPause += kind == 0x12;
            std::printf("cutlist %d: %d commands, %d lines, %d shots, %d pauses\n", up->oid, CScptCommands(up.get()), cLine, cCam, cPause);
        }
    }
    for (const auto& up : psw->aploAll)
        if (up->oid == oid && FIsLoDerivedFrom(up.get(), 0x72)) return up.get();
    return nullptr;
}

void UpdateCutsceneTools(SW* psw, int frame)
{
    TOOLS& t = Tools();
    if (!psw) return;
    if (t.oidTrace && !t.fDone)
    {
        if (!t.fStarted)
        {
            // (the script looked for each step until the world that has it is loaded; started frameStart steps later)
            if (!t.pscpt && (t.pscpt = PscptFind(psw, t.oidTrace))) t.frameFound = frame;
            if (!t.pscpt && frame > 3000)
            {
                std::fprintf(stderr, "cutscene: no script %d in this world\n", t.oidTrace);
                t.fDone = true;
                WriteTrace(psw);
            }
            else if (t.pscpt && frame >= t.frameFound + t.frameStart &&
                     ((IdScptCutscene() == 0 && g_grfjoyLock == 0) || frame >= t.frameFound + t.frameStart + 3600))
            {
                // (the world's own start scenes over first: the pad free, no scene playing; at most a minute later)
                g_pfnScptTrace = OnScptTrace;
                t.fStarted = true;
                t.pswStart = psw;
                t.t0 = TNow();
                for (ASEGA* pa : g_apasega) t.setAmbient.insert(pa);   // (the animations already playing: not the scene's)
                // (a camera object's view the world started with, e.g. the safe house's, given up: the scene's own
                // shots are seen as when it plays outside)
                RevokeCmPolicy(&g_cm, 2, 10, &g_cmpolicies.cplookat, nullptr, nullptr);
                TraceStep(psw);   // (the state before it starts: the first keys)
                StartScpt(t.pscpt, PloPoCurrent(psw));
                std::printf("cutscene: script %d started at frame %d\n", t.oidTrace, frame);
            }
        }
        else if (t.fStarted)
        {
            if (psw == t.pswStart) TraceStep(psw);
            if (psw != t.pswStart && !t.fLeft)
            {
                // (the scene left the world: a transition; its script is gone with the old world)
                t.fLeft = true;
                t.tEnd = R4(TNow() - t.t0);
                t.frameStopped = frame;
            }
            if (!t.fLeft && t.tEnd < 0.0f && TNow() - t.t0 > t.tMax)
            {
                // (still running after the longest a scene is traced: e.g. waiting for the player)
                t.fTimedOut = true;
                t.tEnd = R4(TNow() - t.t0);
                t.frameStopped = frame - 20;
            }
            LO* pFocus = t.oidFocus ? PscptFind(psw, t.oidFocus) : t.pscpt;
            const bool fOver = t.oidFocus ? (t.fFocusStarted && !(pFocus && FScptRunning(pFocus))) || !FScptRunning(t.pscpt) : !FScptRunning(t.pscpt);
            if (fOver && t.tEnd < 0.0f && !t.fLeft && !t.fTimedOut) { t.tEnd = R4(TNow() - t.t0); t.frameStopped = frame; }
            if (t.fLeft || t.fTimedOut || fOver)
            {
                if (t.tEnd < 0.0f) { t.tEnd = R4(TNow() - t.t0); t.frameStopped = frame; }
                // (a few steps after its end: what it leaves behind settles)
                if (frame - t.frameStopped >= 20)
                {
                    t.fDone = true;
                    g_pfnScptTrace = nullptr;
                    WriteTrace(psw);
                }
            }
        }
    }
    if (t.oidPlay && !t.fPlayStarted && frame >= 60 && (PscptFind(psw, t.oidPlay) || frame > 1200))
    {
        t.fPlayStarted = true;
        if (LO* p = PscptFind(psw, t.oidPlay))
        {
            StartScpt(p, PloPoCurrent(psw));
            t.pscpt = t.pscpt ? t.pscpt : p;
            t.fSeeking = t.tSeek > 0.0f;
            std::printf("cutscene: playing script %d%s\n", t.oidPlay, t.fSeeking ? " (fast-forward to the playhead)" : "");
        }
        else std::fprintf(stderr, "cutscene: no script %d in this world\n", t.oidPlay);
    }
    if (t.fSeeking)
    {
        LO* p = PscptFind(psw, t.oidPlay);
        if (!p || !FScptRunning(p) || TScptOf(p) >= t.tSeek)
        {
            t.fSeeking = false;
            std::printf("cutscene: at %.2f s of script %d (frame %d)\n", p ? TScptOf(p) : -1.0f, t.oidPlay, frame);
        }
    }
    if (!t.record.empty()) RecordStep(psw, frame);
}

bool FCutsceneToolsDone() { return Tools().oidTrace && Tools().fDone; }
bool FCutsceneSeeking() { return Tools().fSeeking; }

void FinishCutsceneTools(SW* psw)
{
    TOOLS& t = Tools();
    if (t.oidTrace) WriteTrace(psw);
    if (!t.record.empty() && !t.arec.empty())
    {
        nlohmann::json j;
        j["format"] = "sly2-play-session";
        j["version"] = 1;
        j["columns"] = {"t", "oid", "x", "y", "z", "heading", "speed", "vz", "state", "anim", "stick_x", "stick_y", "buttons"};
        j["steps"] = t.arec;
        std::ofstream f(t.record, std::ios::binary);
        f << j.dump();
        std::printf("cutscene: play session of %zu steps written to %s\n", t.arec.size(), t.record.c_str());
        t.arec.clear();
    }
}

}  // namespace cut
