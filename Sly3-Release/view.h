#pragma once

#ifndef SLY2N_DEV_ROOT  // the development checkout (CMakeLists.txt); only the developer options read below it
#define SLY2N_DEV_ROOT "."
#endif

#include <string>

struct VIEWOPTS
{
    int iworld = 2;
    std::string loadmaps = SLY2N_DEV_ROOT "/build/loadmaps";
    std::string screenshot;
    std::string camera;      // x,y,z,yaw,pitch (degrees)
    std::string cameraRam;   // take the camera from this EE RAM dump (e.g. a PCSX2 savestate's eeMemory.bin)
    int width = 1280, height = 960;
    int msaa = 4;
    bool fNoCel = false;
    bool fFromMap = false;   // the standalone scene (parse map + reference RAM) instead of the world loader
    bool fNoFog = false;
    bool fNoShadow = false;
    float tAnim = 0;
    std::string textTest;    // --text-test: draw this string in the world's font
    float textSize = 32;
    std::string screens;     // --screens OID[:PART],...: play these screen objects natively (part id, default 0x30F then 0x310)
    std::string hudRam;
    int cframeBench = 0;     // --bench N: render N frames, print the time per frame
    float fade = 0, letterbox = 0, wipe = 0;      // --hud-ram: draw the HUD of this EE RAM state over the scene         // screenshots: run the shader animations this long first
    bool fRamLights = false;
    float tParticles = 0;
    int oidBurst = -1;           // --burst OID: explode these explosions first        // --particles T: run the emitters T seconds before the screenshot     // the lights' records from the reference RAM (final.ram) instead of natively
    bool fApproxLight = false;   // the old stand-in instead of the native bake / forward lighting
};

int ViewWorld(const VIEWOPTS& vo);
