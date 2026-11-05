#pragma once

// Movies in the game window (Sly1-Release's fmv path: the picture as a full-screen textured quad, letterboxed to
// the PS2's 4:3 display): a presenter for fmv.h's FPlayMovie that draws into the GL window (gl.h).

#include "fmv.h"

#include <cstdint>
#include <string>

struct MOVIEGL
{
    unsigned tex = 0;
    int cx = 0, cy = 0;
    bool fWindowed = true;        // swap to the window (false: render into the scene target only)
    std::string screenshot;       // write the picture shown at tScreenshot seconds to this PNG, then stop
    double tScreenshot = 0;
    int cframe = 0;
    double fps = 30000.0 / 1001.0;
};

// PFNMOVIEPRESENT: uploads and draws the picture (null: polls the window; Esc / Enter / Space / Start skip).
bool PresentMovieGl(const uint8_t* prgba, int cx, int cy, void* pv);

// Draws an RGBA picture letterboxed (4:3) into the current scene target (no swap).
void DrawMoviePicture(MOVIEGL& m, const uint8_t* prgba, int cx, int cy);

// --view-movie ID [--screenshot file.png --time T]: plays a movie in the GL window.
int ViewMovieCommand(const std::string& arg, const std::string& screenshot, double t, int width, int height);
