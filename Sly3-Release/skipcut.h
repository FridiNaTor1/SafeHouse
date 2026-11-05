#pragma once

// Skip cutscenes (Phase 3 QoL, USER_BUGS Phase 3 item 8). Nothing shows until a button is pressed during a
// cutscene; then a small "Hold (O) to skip" hint. Holding Circle (the bound button: B on Xbox / Steam, the dynamic
// prompts of promptglyph.h) for 3 s skips, while an indicator (the button's icon with a line circling it) fills.
// With "Instant Skip Cutscenes" (off by default) one press of Circle skips.
//
// A skip never cuts anything: the game itself runs to the cutscene's natural end, fast (many 60 Hz steps per
// displayed frame, sound muted, its dialog lines timed by the frame clock instead of the audio device), so the
// tasks, positions and music end exactly as at the natural end. What counts as a cutscene: a talk (the pad's talk
// lock: the scripts' scenes with their pause, command 0x12, and the binocucom talks) and a job's closing sequence;
// a pre-rendered movie (no game state) just stops, as at its end. A scene that waits for a button press (SCPT
// command 0x42) is not skippable.

#include <cstdint>

struct GLFWwindow;

// true while the fast-forward runs (sound.cpp: the dialog by the frame clock)
extern bool g_fSkipCutFast;

// a cutscene now (the game's own state; nothing of the skip's)
bool FCutsceneActive();
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

// Movies (moviegl.cpp's presenter): this frame's input from the window; true: skip the movie now. Draws the hint /
// indicator into the frame (call after the picture).
bool FMovieSkip(GLFWwindow* window, float dtReal);
void ResetMovieSkip();
void DrawMovieSkip();
