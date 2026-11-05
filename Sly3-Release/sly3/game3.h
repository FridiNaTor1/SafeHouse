#pragma once

// Sly 3's game loop (the Sly 3 class loaders): the boot (orig3 0x1C6EA8: the logos, then world 35 m_ext with point 0x1A8),
// the transition that loads a world (0x289070 queues, 0x25C2A8 runs it at the frame's start), the world start
// (StartSw 0x25B860: the cameras 0x165748, the fixed objects 0x173C10, 0x25B130's 0x8A, the splice modules), and
// the frame of the main loop 0x1CB1D8 (REPORT §2.3): transition, clock 0x165430, input, world update 0x27D7E8 (the
// per-object updates registered here, the cameras 0x27DE38), render 0x209C40 through scene3's renderer from the
// world's camera (SW +0x463C), present.
//
// Interfaces for the other parts (the Sly 3 script port, the Sly 3 player, the Sly 3 renderer): g_game3 (the world, its scene and
// cameras), RegisterUpdate3 (a class's per-frame update, as the original's update lists), RegisterFrameHook3 (code
// run once per frame in the world update, e.g. the SCPT runner), QueueWorld3 (the transition), CM3 (a camera's state:
// set pos / mat / fov to move the view).

#include "brx3.h"
#include "scene3.h"
#include "../clock.h"
#include "../joy.h"
#include "../scene.h"

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct SW;
struct LO;

// A camera (class 0x05, 0x450 bytes in RAM; vt 0x3557E8): the fields the renderer needs.
struct CM3
{
    LO* plo = nullptr;              // the object (5756 in m_ext: the world's; 4 / 5: StartupCm 0x165748's)
    glm::vec3 pos{0.0f};            // +0x80
    glm::mat3 mat{1.0f};            // +0x50 / +0x60 / +0x70: the rows (view direction, left, up)
    float tanY = 0.0f;              // +0x13C (0: from fovDeg)
    float fovDeg = 70.0f;           // 0x1679E0's argument (StartupCm: 70)
    float sNear = 50.0f, sFar = 150000.0f;  // +0x144 / +0x148 (0x167A50 sets far)
    int arcViewport[4] = {0, 0, 0, 0};      // 0x167BF0 (oids 4 / 5: the picture-in-picture views)
    int mode = 0;                   // 0x168478's argument
};

struct GAME3OPTS
{
    bool fHeadless = false;         // no window (a hidden GL context renders the frames)
    bool fSkipLogos = false;        // --skip-logos
    int cframe = -1;                // --frames N (-1: until the window closes)
    std::string screenshot;         // --screenshot: the last frame
    std::string frameDump;          // --frame-dump DIR[:EVERY]
    int width = 1280, height = 960;
    int iworld = -1;                // --world3 W (default: the boot's world 35)
    bool fFixedDt = false;
    std::string census;             // --census --out FILE: the object census after the world start (census3.py)
    std::vector<JOYPRESS> apress;   // --press (frames of the world loop; --press-vblank-base subtracted)
};

struct GAME3
{
    std::unique_ptr<SW> psw;
    std::vector<uint8_t> abWorld;
    WORLDWALK3 walk;
    SCENE scene;
    SCENE3STATS sceneStats;                   // the scene's build (its entries' placed records: anim3's poses)
    bool fSceneGpu = false;
    int iworld = -1;
    int iworldPending = -1, pointPending = -1;  // 0x289070's queue
    int cframe = 0;
    std::vector<std::unique_ptr<CM3>> acm;    // every camera made (StartupCm's and the world's)
    std::vector<CM3*> apcmUpdate;             // SW +0x3EB8 / +0x3EBC: the cameras 0x27DE38 updates
    CM3* pcmMain = nullptr;                   // SW +0x463C (the world's camera when it has exactly one)
    CM3* pcm4 = nullptr, *pcm5 = nullptr;     // SW +0x4640 / +0x4644
};
extern GAME3 g_game3;

CM3* PcmFromLo3(const LO* plo);
using PFNUPDATE3 = std::function<void(LO*, float dt)>;
void RegisterUpdate3(int cid, PFNUPDATE3 pfn);   // objects of cid and its derived classes, each frame
void RegisterFrameHook3(std::function<void(float dt)> pfn);
void QueueWorld3(int iworld, int point);         // orig3 0x289070(GAME, 0x25E070(iworld), point, iworld, -1)

// The SCPT executor's camera command 0x45 (pos, euler {roll, -tilt, pan}, vertical fov in radians)
void ApplyCmCutFromScpt3(CM3* pcm, const glm::vec3& pos, const glm::vec3& euler, float fovRad);
// Set by the SCPT executor when it runs (the Sly 3 script port): the world start then leaves the camera to the scripts.
extern bool g_fScptRunner3;
constexpr int SCPT_ATTRACT_M_EXT = 4262;   // m_ext's attract script (group 390; its cmd 21 is the first camera command)

int RunGame3(const GAME3OPTS& o);

// The main loop's input step (orig3 0x1CB1D8: the pads are read after the clock 0x165430, before the world update):
// UpdateJoy on Sly 3's clock (g_pclockJoy = &g_clock3). A route (refroute3 syntax: JOYPRESS frames counted from the
// world start, as g_game3.cframe) replaces the window's input: SetInputRoute3.
struct JOYPRESS;
void SetInputRoute3(const std::vector<JOYPRESS>& apress);
