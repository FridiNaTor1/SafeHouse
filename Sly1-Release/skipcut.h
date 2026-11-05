#pragma once
// Skip cutscenes (PC QoL, as Sly2-Native's skipcut.h; USER_BUGS Phase 3 items 4 / 8).
// Nothing shows until a button is pressed during a cutscene; then a small
// "Hold (O) to skip" hint. Holding Circle (the bound button, drawn with the
// dynamic prompts of prompt_glyphs.h) for 3 s skips, while an indicator (the
// button's icon with a line circling it) fills. With "Instant Skip Cutscenes"
// (off by default) one press of Circle skips.
//
// A skip never cuts anything: the game itself runs to the cutscene's natural
// end, fast (many steps per displayed frame, the output muted, the dialogue
// lines timed by the game clock instead of the audio device), so the level
// state, positions and music end exactly as at the natural end. A cutscene is
// a letterboxed scene (SW cLetterbox, the scripted scenes and conversations)
// with no menu up. A movie that follows the scene (or a level change) ends the
// fast-forward and then plays as usual. Pre-rendered movies (fmv.cpp) keep
// their original skip buttons and also take the Circle hold.

#include <cstdint>

struct GLFWwindow;

// A cutscene now (the game's own state; nothing of the skip's).
bool FCutsceneActive();
// Once per displayed frame after the input update: the hint, the hold, the
// skip's start. Hides the Circle that skipped from the game until released.
void UpdateSkipCut(float dtReal);
// The game loop runs more steps this frame (the cutscene is being skipped).
bool FSkipCutFastForward();
// After a fast-forward step: false when the skip is over (the cutscene ended,
// a movie or level change is pending, or the time cap was reached).
bool FSkipCutStep();
// The hint and the indicator (after the UI is drawn).
void DrawSkipCut();

// Movies (fmv.cpp): this frame's input (g_joy updated); true: end the movie now.
bool FMovieSkipHeld(float dtReal);
void ResetMovieSkip();
void DrawMovieSkip();
