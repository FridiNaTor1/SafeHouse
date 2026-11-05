#pragma once

#ifndef SLY2N_DEV_ROOT  // the development checkout (CMakeLists.txt); only the developer options read below it
#define SLY2N_DEV_ROOT "."
#endif

#include "joy.h"

#include <cstdio>
#include <string>
#include <vector>

struct SW;

extern bool g_fDevCard;   // the development memory card as the fallback save (off when started by the launcher)
struct GAMEOPTS
{
    std::string world = "2";        // a world record index (Paris hub: 2), "boot" or "frontend"
    int task = -1;                  // a task (job) start: its oid (the reference's --warp W:TASK)
    int cframe = -1;                // stop after this many frames (-1: until the window closes)
    bool fHeadless = false;         // no window (the differential harness)
    bool fFixedDt = false;          // every frame 1/60 s
    std::vector<JOYPRESS> apress;   // scripted input
    std::string frameDump;          // per-frame object states (the reference's SLY2_FRAMEDUMP format)
    std::string screenshot;         // the last frame to a PNG
    std::string cameraRam;          // debug: the camera of an EE RAM dump (a savestate's) for the pictures
    std::string camera;             // debug: a fixed camera "x,y,z,yaw,pitch" for the pictures (not the game's)
    std::string loadmaps = SLY2N_DEV_ROOT "/build/loadmaps";  // the reference's RAM (camera, lights)
    int width = 1280, height = 960;
    std::string save;               // a save slot file (default: <user-dir>/Saves/save1, else the dev card's save1)
    std::vector<std::string> aprintProgress;  // progress paths to print after the load (dot-separated)
    std::string gangStart, gangRef;  // Bentley's / Murray's harness (gangcheck.cpp)
    std::string slyStart, slyRef;    // the actors' harness (slycheck.cpp): the reference's RAM and trace
    int vbBase = 0;                  // the reference's vblank of the native frame 0 (the harness)
};

int RunGame(const GAMEOPTS& go);
void WriteFrameDump(FILE* pf, const SW* psw, int frame);
