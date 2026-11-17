#pragma once

// Skip cutscenes (Phase 3 QoL, USER_BUGS Phase 3 item 8). Nothing shows until a button is pressed during a
// cutscene; then a small "Hold (O) to skip" hint. Holding Circle (the bound button: B on Xbox / Steam, the dynamic
// prompts of promptglyph.h) for 2 s skips, while an indicator (the button's icon with a line circling it) fills.
// With "Instant Skip Cutscenes" (off by default) one press of Circle skips. A decided skip shows one frame with the
// indicator full, then skips.
//
// A skip never cuts anything: the game itself runs to the cutscene's natural end, fast (its 60 Hz steps, sound
// muted, its dialog lines timed by the frame clock instead of the audio device), so the tasks, positions, spawned
// objects and music end exactly as at the natural end (tools/skipcheck.py compares them). The whole fast-forward runs
// within one displayed frame (game.cpp, up to 5 s; about 0.5 to 1 s for a minute of scene on a PC): the last picture stays up, the
// next one is the scene's end with the normal camera and HUD, with no blackout; only a longer one (a stuck scene)
// goes on behind a fade. A scene that starts while the pad is still locked after the skipped one (in the same step, or
// within 60 steps with the pad locked all along: a scene in several scripts, a talk straight after a cinematic) is
// skipped too, as the same cutscene (skipcut.cpp FChainScene). (The PS4 / PS5
// classic's skip, andshrew/Sly2-Cutscene-Skip, ends the scripts' dialogue waits early instead (SCPT 0x3B / 0x45 /
// 0x52 for 30 steps): instant, but the world and the scripted walks do not wait, hence its known softlocks (Ep 4's
// train hack trigger not spawned, Ep 6's chopper out of place, job ends skipped on their first frame). Ending each
// line at once here gave the same kind of differences in skipcheck: notes/SKIP.md.) Eligible scenes: binocucom
// conversations, and the scenes of scripts that pause the game (SCPT 0x12) with a dialogue line or a binocucom talk
// in them or in the scripts they start, or a cinematic of three or more camera shots (scpt.h SceneSkipKind). Pad
// locks alone (doors, safehouse transitions, signs, job banners) and one- or two-shot camera moments (a gate shown
// opening) are not scenes; a pre-rendered movie (no game state) just stops, as at its end. A skip stops where a
// scene waits for a button press (SCPT command 0x42).

#include <cstdint>

struct GLFWwindow;

// true while the fast-forward runs (sound.cpp: the dialog by the frame clock)
extern bool g_fSkipCutFast;

// a cutscene now (the game's own state; nothing of the skip's)
bool FCutsceneActive();
// a script waited for a button within the last steps (the scene needs the pad: pcinput.cpp's touch layout)
bool FSkipCutWaitsForButton();
// Native QoL identity for each new authored scene. Never reused across world loads.
uint64_t NewSkipCutScene();
// Once per displayed frame, after the frame's steps: the hint, the hold, the skip's start and end. dtReal: the
// real time since the last call; grfHeld / grfPressed: the native pad (joy.h BTN_* layout).
void UpdateSkipCut(float dtReal, uint16_t grfHeld, uint16_t grfPressed);
// The game loop runs more steps this frame (the cutscene is being skipped).
bool FSkipCutFastForward();
// One fast-forward step ran: false when the skip is over (the cutscene ended, or it waits for input).
bool FSkipCutStep();
// The buttons the game must not see (Circle held from a skip until it is released).
uint16_t GrfSkipCutMask();
// The hint and the indicator (the HUD pass, over everything).
void DrawSkipCut();
// (scpt.cpp) a runner waits for a button this step (command 0x42)
void NoteScptWaitsForButton();

// (test: SLY2N_SKIPCHECK=N:OFF:K:FILE[:skip]) the N-th scene of the run (1: the first), OFF frames
// after it began, is followed to its end (with ":skip" it is skipped there, as after the hold); K frames after its end
// (and again 30 frames later) the end state goes to FILE.1 / FILE.2: the tasks, the player, the camera, the pause
// counts, the music, every root object's place. A watched run and a skipped one compare the skip's end with the
// natural one.

// Movies (moviegl.cpp's presenter): this frame's input from the window; true: skip the movie now. Draws the hint /
// indicator into the frame (call after the picture).
bool FMovieSkip(GLFWwindow* window, float dtReal);
void ResetMovieSkip();
void DrawMovieSkip();
void FMovieSkipTest();   // (test: SLY2N_MOVIESKIPAT, moviegl.cpp)
