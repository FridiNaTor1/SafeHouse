// The reference's controller waypoint programs (the reference's route format)
// replayed natively (debug, SLY2N_ROUTE=file@tT or file@F: the program's epoch at game time T or frame F): the same
// file drives both games' pads, so their positions can be compared. Directives: after-warp N, move X Y NEAR BUDGET
// ZMIN ZMAX run|walk [strength=G], wait N, input N CSV, aim N X Y run|walk CSV [strength=G]. The stick toward a world
// point through the camera's view direction (CM +0x60's x row, the reference's 0x2DDFA0), run = R1 beyond 170.
// Feedback on another object (a guard's patrol pauses at random): waitobj CID OID X Y R N (neutral pad until an in-world
// object of that class / oid is within R of X,Y; fails after N frames), chase CID OID NEAR BUDGET ZMIN ZMAX run|walk
// [strength=G] [jump=N] [within=R] [follow] [maxhp: the guard with the most full health first] [press=CSV: those presses every 60 frames, e.g. a vehicle's drive and fire] (move toward the nearest such object until within NEAR; jump=N: a double jump
// every N frames; within=R: only objects within R of the player, none: the step is over; follow: stay within NEAR of it,
// neutral while there, for the whole budget N frames, which then ends the step instead of failing it).
// point N X Y Z CSV [obj=CID:OID:DZ] [until=SCPT] [every=F]: N frames of the left stick turning the camera (e.g. the binocucom's view) toward the
// world point (or the nearest in-world object CID / OID to X,Y, DZ above it; lo=OID[:DZ]: that object's world position), the CSV's presses on top (RY zoom, R1);
// until=SCPT: the step ends once that script starts (N: the budget); every=F: the presses repeat every F frames (a gun fired until a hit);
// aimed=RAD: the button presses wait while the view is off the target by more than RAD (yaw or pitch; the stick values still apply).
// waitscpt OID N [CSV]: until script OID starts again (N frames, then on), the CSV's presses meanwhile.
// waitpo OID N: until the current player is OID (a vehicle a script hands the pad to; N frames, then on).
// setposlo OID[,OID..] X Y R ZMIN ZMAX [DZ [OFF]]: as setpos, DZ above (OFF toward X,Y from) the nearest in-world copy of those oids to X,Y (within R,
// its height in ZMIN..ZMAX; e.g. a carryable that respawns elsewhere).
// setpos X Y Z: the current player placed there by its position setter (vt slot 29), velocities zeroed (both engines:
// the reference's route setpos = SLY2_SETPOS on 3DD490+3EFC@); one frame.
// teleport X Y Z [H]: the current player put there (as SLY2N_TELEPORT: Z is the object's, H the heading, camera cut) at the
// step's start (one frame); waittask OID STATE N [CSV]: until the task's state is >= STATE (after N frames it goes on), the CSV's
// presses meanwhile (e.g. CROSS taps through a dialog); skipif OID STATE N: the next N steps skipped when the task's state is
// >= STATE (native only: e.g. a fight's remaining rounds once it is won); taskreq OID STATE: a poke (as SLY2N_TASKREQ); intovol OID [DZ]: the player at a volume's center (its floor + DZ); pocket OID N [DZ] [until=TASK]: N frames, the player kept in guard OID's pickpocket volume at its point nearest the pocket, CIRCLE once the pickpocket can start, until the pocket is taken; tail CID OID DIST N [DZ] [CSV]: N frames (the CSV's presses every 60 frames), the player put DIST/2 behind the nearest such object whenever farther than DIST; onlo OID [DZ] [N] [DX DY [H]]: the player onto the N-th in-world copy of OID, DZ above, with its velocity (a moving train); giver OID [D]: as SLY2N_TELEGIVER (-1: the first guard with a prize, -N: the N-th); waitclock DT N: until the game clock ran DT (popups pause it); hackdock X Y: the hack ship put there once driven; warpworld N[:TASK]: world record N loaded in this process (the debug menu's warp; :TASK at that task's start, as the episode chooser); nojt OID[,OID..] N [CSV]: N frames (the CSV's presses meanwhile), failing once one of those objects is a tracked interaction target (a prompt); all independent of the game clock (pauses, scenes).
#include "activate.h"
#include "gadget.h"
#include "game/items3.h"
#include "game/sly3.h"
#include "crv.h"
#include "actor.h"
#include "alo.h"
#include "damage.h"
#include "guard.h"
#include "hack.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "joy.h"
#include "jt.h"
#include "lo.h"
#include "po.h"
#include "so.h"
#include "sw.h"
#include "task.h"
#include "game/scptgame3.h"
#include "game/task3.h"
#include "transition.h"
#include "routetele.h"
#include "vtnative.h"
#include "worldaccess.h"
#include "aipassword3.h"
#include "outfit3.h"
#include "photo.h"
#include "sly.h"
#include "pocket.h"
static bool g_fRoutePocketPress = false;
bool FPointInsideShape(LO* plo, const glm::vec3& pos);   // detect.cpp

#include <algorithm>
#include <deque>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <string>
#include <vector>

bool FDogfightStick3(LO* plo, float* px, float* py, bool* pfFire, int* pnSpeed);   // game/biplane3.cpp (the "dogfight" step)
bool FRowStick3(LO* plo, int oidTask, float* px, float* py, bool* pfStroke);       // game/rowboat3.cpp (the "row" step)
bool FSailToward3(LO* plo, const glm::vec3& pos, unsigned* pgrf, float* pdist);   // game/ship3.cpp (the "sail" step)
bool FShipBroadside3(LO* plo, glm::vec3* ppos, float* pheading, glm::vec3* pvel, unsigned* pgrf, bool* pfUse);   // (the "broadside" step)

ROUTETELE g_routetele;
ROUTEHACKDOCK g_routehackdock;

namespace
{
struct PRESSR { int at = 1, c = 1, axis = -1; uint16_t grf = 0; uint8_t b = 0x80; };
struct STEPR
{
    enum { Move, Wait, Input, Aim, WaitObj, Chase, Point, Teleport, WaitTask, TaskReq, WaitClock, Giver, IntoVol, SetPos, OnLo, Tail, SkipIf, SetPosLo, WaitScpt, HackDock, NoJt, WarpWorld, StartScpt, Wear, Password, Photo, Gadget, WaitPo, DrivePath, Dogfight, Row, Sail, Broadside, WaitCmd, HoldBtn } kind = Wait;
    float h = 0; bool fH = false;
    int cid = 0, oid = 0;
    int frames = 0;
    float x = 0, y = 0, z = 0, dz = 0, near = 0, zmin = -1e30f, zmax = 1e30f, strength = 1.0f;
    float within = 0.0f;  // (chase within=R: targets beyond R of the player ignored)
    int jump = 0;  // (chase jump=N: Cross at the step's frames N*k+1..+6 and +13..+18, as the reference's)
    bool follow = false;  // (chase follow: within NEAR the pad is neutral and the step goes on until its budget)
    bool maxhp = false;   // (chase maxhp: of the candidates, the guard with the most full health first (a boss among its crew))
    float behind = 0.0f;  // (chase behind=R: only objects held by a hack enemy (cid 0x4B, e.g. Sly 3's hack 4 claws' balls),
                          // reached from their holder's back: around the holder at R while on its front side)
    bool run = false;
    bool fPocket = false;  // (pocket: a tail into the guard's pickpocket volume)
    bool f3d = false;      // (tail 3d: the distance in 3-D, e.g. a player who climbed above the tailed guard)
    bool fLock = false;    // (tail lock: the object chosen at the first frame followed, not the nearest copy each frame)
    int cTrail = 0;        // (tail trail=K: put where the object's origin was K frames before (ground it stood on), not behind it)
    int oidUntil = -1;  // (point until=OID: the step ends once script OID starts; its frames are then the budget)
    int every = 0;      // (point every=N: the CSV's presses repeat every N frames)
    float aimed = -1.0f;  // (point aimed=RAD: the CSV's button presses (not its stick values) held back while the aim is off by more)
    int fRight = 0;     // (look: point with the right stick, the follow camera; SLY3_LOOKSIGN=-1 flips the pan)
    std::vector<int> aoid;  // (setposlo: the oids, any of them)
    std::vector<PRESSR> apress;
    // (move / wait / input fire=OID[,OID..] | fire=@X:Y [firerange=R] [fireprio [firenear=R2]]: the right stick toward the nearest in-world
    // object of those oids within R (default 1500) of the player (fireprio: of the first oid in the list that has one in
    // range; firenear: any within R2 before those), or toward the world point, while the step runs: a hack ship's guns; untilclear: the step ends once none is left in range;
    // wait hold=X:Y: the left stick keeps the player near X,Y meanwhile; wait yaw=OID:A0:A1: ends once OID's heading (degrees
    // mod 180) is within A0..A1)
    std::vector<int> aoidFire;
    bool fFirePrio = false;   // (fireprio: the first oid of the list with an object in range, the nearest of those)
    float rFireNear = 0.0f;   // (firenear=R: with fireprio, any listed object within R first: the chasers that close in)
    bool fUntilClear = false; // (untilclear: the step ends once no listed object is left to fire at (after 30 frames))
    bool fHold = false;       // (wait hold=X:Y: the stick keeps the player near that point while it waits (a hack's pull))
    float xHold = 0.0f, yHold = 0.0f;
    int oidYaw = -1;          // (wait yaw=OID:A0:A1: the wait ends once that object's heading, degrees mod 180 (a bar's two
    float aYaw0 = 0.0f, aYaw1 = 0.0f;  // arms), is within A0..A1: a hack's spinning bar timed)
    bool fFire = false, fFirePoint = false;
    float xFire = 0, yFire = 0, rFire = 1500.0f;
};
struct ROUTE
{
    bool fLoaded = false, fStarted = false, fDone = false, fFailed = false, fAdvance = false;
    int after = 0, istep = 0, frameStart = -1, frameStep = 0, frameEpoch = -1;
    float tEpoch = -1.0f;
    std::vector<STEPR> astep;
    std::FILE* pfTrace = nullptr;
    uint16_t grfHeld = 0;   // (holdbtn: buttons kept down from step to step)
};
ROUTE& Route()
{
    static ROUTE s_r;
    return s_r;
}

bool FParsePresses(const std::string& csv, std::vector<PRESSR>& a)
{
    if (csv == "-") return true;
    std::vector<JOYPRESS> ap;
    if (!FParseJoyPresses(csv, ap)) return false;
    for (const JOYPRESS& p : ap) a.push_back({p.frame, p.c, p.axis, p.grfbtn, p.bAxis});
    return true;
}

void Load()
{
    ROUTE& r = Route();
    r.fLoaded = true;
    const char* sz = std::getenv("SLY2N_ROUTE");
    if (!sz) return;
    std::string spec(sz), path = spec;
    if (size_t at = spec.rfind('@'); at != std::string::npos)
    {
        path = spec.substr(0, at);
        const std::string e = spec.substr(at + 1);
        if (!e.empty() && e[0] == 't') r.tEpoch = std::strtof(e.c_str() + 1, nullptr);
        else r.frameEpoch = std::atoi(e.c_str());
    }
    std::ifstream in(path);
    std::string line;
    bool fHeader = false;
    while (std::getline(in, line))
    {
        if (size_t h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        std::vector<std::string> f;
        for (std::string w; ss >> w;) f.push_back(w);
        if (f.empty()) continue;
        if (!fHeader)
        {
            fHeader = f[0] == "after-warp" && f.size() == 2;
            r.after = fHeader ? std::atoi(f[1].c_str()) : 0;
            continue;
        }
        STEPR s;
        auto strength = [&](size_t i) { if (f.size() > i && f[i].rfind("strength=", 0) == 0) s.strength = std::strtof(f[i].c_str() + 9, nullptr); };
        if (f[0] == "move" && f.size() >= 8)
        {
            s.kind = STEPR::Move;
            s.x = std::strtof(f[1].c_str(), nullptr); s.y = std::strtof(f[2].c_str(), nullptr);
            s.near = std::strtof(f[3].c_str(), nullptr); s.frames = std::atoi(f[4].c_str());
            s.zmin = std::strtof(f[5].c_str(), nullptr); s.zmax = std::strtof(f[6].c_str(), nullptr);
            s.run = f[7] == "run";
            strength(8);
        }
        else if (f[0] == "waitobj" && f.size() >= 7)
        {
            s.kind = STEPR::WaitObj;
            s.cid = static_cast<int>(std::strtol(f[1].c_str(), nullptr, 0)); s.oid = std::atoi(f[2].c_str());
            s.x = std::strtof(f[3].c_str(), nullptr); s.y = std::strtof(f[4].c_str(), nullptr);
            s.near = std::strtof(f[5].c_str(), nullptr); s.frames = std::atoi(f[6].c_str());
        }
        else if (f[0] == "chase" && f.size() >= 8)
        {
            s.kind = STEPR::Chase;
            s.cid = static_cast<int>(std::strtol(f[1].c_str(), nullptr, 0)); s.oid = std::atoi(f[2].c_str());
            s.near = std::strtof(f[3].c_str(), nullptr); s.frames = std::atoi(f[4].c_str());
            s.zmin = std::strtof(f[5].c_str(), nullptr); s.zmax = std::strtof(f[6].c_str(), nullptr);
            s.run = f[7] == "run";
            strength(8);
            for (size_t i = 8; i < f.size(); ++i)
            {
                if (f[i].rfind("jump=", 0) == 0) s.jump = std::atoi(f[i].c_str() + 5);
                else if (f[i].rfind("within=", 0) == 0) s.within = std::strtof(f[i].c_str() + 7, nullptr);
                else if (f[i] == "follow") s.follow = true;
                else if (f[i] == "maxhp") s.maxhp = true;
                else if (f[i].rfind("behind=", 0) == 0) s.behind = std::strtof(f[i].c_str() + 7, nullptr);
                else if (f[i].rfind("press=", 0) == 0) FParsePresses(f[i].substr(6), s.apress);  // (every 60 frames)
                else strength(i);
            }
        }
        else if (f[0] == "wait" && f.size() >= 2) { s.kind = STEPR::Wait; s.frames = std::atoi(f[1].c_str()); }
        else if (f[0] == "nojt" && f.size() >= 3)
        {
            // nojt OID[,OID..] N [CSV]: N frames (the CSV's presses meanwhile), failing once any of those objects is a
            // tracked interaction target (its prompt shown / CIRCLE able to take it: g_jttrack)
            s.kind = STEPR::NoJt; s.frames = std::atoi(f[2].c_str());
            for (const char* q = f[1].c_str(); q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr) s.aoid.push_back(std::atoi(q));
            if (f.size() >= 4) FParsePresses(f[3], s.apress);
        }
        else if (f[0] == "input" && f.size() >= 3) { s.kind = STEPR::Input; s.frames = std::atoi(f[1].c_str()); FParsePresses(f[2], s.apress); }
        else if (f[0] == "aim" && f.size() >= 6)
        {
            s.kind = STEPR::Aim;
            s.frames = std::atoi(f[1].c_str());
            s.x = std::strtof(f[2].c_str(), nullptr); s.y = std::strtof(f[3].c_str(), nullptr);
            s.run = f[4] == "run";
            FParsePresses(f[5], s.apress);
            strength(6);
        }
        else if ((f[0] == "point" || f[0] == "look") && f.size() >= 6)
        {
            s.kind = STEPR::Point;
            s.fRight = f[0] == "look";
            s.frames = std::atoi(f[1].c_str());
            s.x = std::strtof(f[2].c_str(), nullptr); s.y = std::strtof(f[3].c_str(), nullptr); s.z = std::strtof(f[4].c_str(), nullptr);
            FParsePresses(f[5], s.apress);
            s.cid = -1;
            if (f.size() >= 7 && f[6].rfind("obj=", 0) == 0)
                std::sscanf(f[6].c_str() + 4, "%i:%d:%f", &s.cid, &s.oid, &s.dz);
            else if (f.size() >= 7 && f[6].rfind("lo=", 0) == 0)  // (any object's world position, e.g. a photo target)
            {
                s.cid = -2;
                std::sscanf(f[6].c_str() + 3, "%d:%f", &s.oid, &s.dz);
            }
            for (size_t i = 7; i < f.size(); ++i)  // (until=OID: until that script starts; every=N: the presses repeat)
            {
                if (f[i].rfind("until=", 0) == 0) s.oidUntil = std::atoi(f[i].c_str() + 6);
                else if (f[i].rfind("every=", 0) == 0) s.every = std::atoi(f[i].c_str() + 6);
                else if (f[i].rfind("aimed=", 0) == 0) s.aimed = std::strtof(f[i].c_str() + 6, nullptr);
            }
        }
        else if (f[0] == "teleport" && f.size() >= 4)
        {
            s.kind = STEPR::Teleport; s.frames = 1;
            s.x = std::strtof(f[1].c_str(), nullptr); s.y = std::strtof(f[2].c_str(), nullptr); s.z = std::strtof(f[3].c_str(), nullptr);
            if (f.size() >= 5) { s.fH = true; s.h = std::strtof(f[4].c_str(), nullptr); }
        }
        else if (f[0] == "hackdock" && (f.size() == 3 || (f.size() == 2 && f[1] == "auto")))
        {
            // hackdock X Y: the hack ship put at (X, Y) (local hack coordinates, as SLY2N_HACKTELE) once it is driven;
            // hackdock auto: at the point of the waiting in-world dock farthest from it (Sly 3's ship, hack3.cpp)
            s.kind = STEPR::HackDock; s.frames = 1;
            s.x = f.size() == 2 ? NAN : std::strtof(f[1].c_str(), nullptr); s.y = f.size() == 2 ? NAN : std::strtof(f[2].c_str(), nullptr);
        }
        else if (f[0] == "setpos" && f.size() == 4)
        {
            s.kind = STEPR::SetPos; s.frames = 1;
            s.x = std::strtof(f[1].c_str(), nullptr); s.y = std::strtof(f[2].c_str(), nullptr); s.z = std::strtof(f[3].c_str(), nullptr);
        }
        else if (f[0] == "setposlo" && f.size() >= 7)
        {
            // setposlo OID[,OID..] X Y R ZMIN ZMAX [DZ [OFF]]: the player set (as setpos) DZ above the in-world copy of any of
            // those oids nearest X,Y (within R, its center in ZMIN..ZMAX): a carryable that respawns elsewhere
            s.kind = STEPR::SetPosLo; s.frames = 1;
            for (const char* q = f[1].c_str(); q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr) s.aoid.push_back(std::atoi(q));
            s.x = std::strtof(f[2].c_str(), nullptr); s.y = std::strtof(f[3].c_str(), nullptr); s.near = std::strtof(f[4].c_str(), nullptr);
            s.zmin = std::strtof(f[5].c_str(), nullptr); s.zmax = std::strtof(f[6].c_str(), nullptr);
            s.dz = f.size() >= 8 ? std::strtof(f[7].c_str(), nullptr) : 100.0f;
            s.h = f.size() >= 9 ? std::strtof(f[8].c_str(), nullptr) : 0.0f;  // (OFF: that far from it toward X,Y)
        }
        else if (f[0] == "waitpo" && f.size() >= 3)
        {
            // waitpo OID N: until the current player is OID (e.g. a vehicle the script hands the pad to; N frames, then on)
            s.kind = STEPR::WaitPo;
            s.oid = std::atoi(f[1].c_str()); s.frames = std::atoi(f[2].c_str());
        }
        else if (f[0] == "drivepath" && f.size() >= 5)
        {
            // drivepath CRV AHEAD NEAR N [CSV]: the current player is a vehicle driven on its pad (X held): its stick
            // steers toward the point AHEAD along the curve CRV past the nearest one, as a path-driven car aims
            // (rccar3.cpp modes 2 / 3); the step ends within NEAR of the curve's end (N frames at most, then on;
            // the CSV's presses meanwhile, e.g. extra buttons)
            s.kind = STEPR::DrivePath;
            s.oid = std::atoi(f[1].c_str()); s.dz = std::strtof(f[2].c_str(), nullptr);
            s.near = std::strtof(f[3].c_str(), nullptr); s.frames = std::atoi(f[4].c_str());
            if (f.size() >= 6) FParsePresses(f[5], s.apress);
        }
        else if (f[0] == "dogfight" && f.size() >= 3)
        {
            // dogfight TASK N: the current player is a plane (class 0x52, game/biplane3.cpp): its stick flies it at the
            // best enemy plane and R1 fires when it is in the sights, until task TASK is done (state >= 2; N frames at most)
            s.kind = STEPR::Dogfight;
            s.oid = std::atoi(f[1].c_str()); s.frames = std::atoi(f[2].c_str());
        }
        else if (f[0] == "row" && f.size() >= 3)
        {
            // row TASK N: the current player is a rowboat (class 0x53, game/rowboat3.cpp): the stick toward the task's next
            // trigger volume and Cross (a stroke) every 16 frames, until task TASK is done (state >= 2; N frames at most)
            s.kind = STEPR::Row;
            s.oid = std::atoi(f[1].c_str()); s.frames = std::atoi(f[2].c_str());
        }
        else if (f[0] == "sail" && f.size() >= 4)
        {
            // sail OID NEAR N [TASK]: the current player sails a ship (class 0x58, game/ship3.cpp; or its character steers
            // it in a battle): L2 / R2 turn it toward object OID (a job's marker), until within NEAR (or task TASK is done,
            // state >= 2; N frames at most)
            s.kind = STEPR::Sail;
            s.cid = std::atoi(f[1].c_str()); s.near = std::strtof(f[2].c_str(), nullptr); s.frames = std::atoi(f[3].c_str());
            s.oid = f.size() >= 5 ? std::atoi(f[4].c_str()) : -1;
        }
        else if (f[0] == "broadside" && f.size() >= 3)
        {
            // broadside TASK N: in a ship battle the player (on the deck of the gang's ship) fires its cannons: every 6 s
            // he is put at the use point of a ready cannon on the side that faces the nearest enemy ship and presses
            // Circle (the cannon's script fires that side, ship method 1997); L2 / R2 keep the enemy abeam; until task
            // TASK is done (state >= 2; N frames at most)
            s.kind = STEPR::Broadside;
            s.oid = std::atoi(f[1].c_str()); s.frames = std::atoi(f[2].c_str());
        }
        else if (f[0] == "waitcmd" && f.size() >= 4)
        {
            // waitcmd OID IDX N (Sly 3): until command IDX of script OID runs again (after the step's start; N frames, then
            // on): e.g. the heist's double door, the guard's pull ([64] of SCPT 2434), the player's press right after it
            // ("waitcmd OID IDX N ever": until it has run at all, a run before the step's start counts too; e.g. the
            // script that adds a photo target, whose start depends on a guard's walk)
            s.kind = STEPR::WaitCmd;
            s.oid = std::atoi(f[1].c_str()); s.cid = std::atoi(f[2].c_str()); s.frames = std::atoi(f[3].c_str());
            s.x = f.size() >= 5 && f[4] == "ever" ? 1.0f : 0.0f;
        }
        else if (f[0] == "waitscpt" && f.size() >= 3)
        {
            // waitscpt OID N: until script OID starts again (a start after the step's own start; N frames, then on)
            s.kind = STEPR::WaitScpt;
            s.oid = std::atoi(f[1].c_str()); s.frames = std::atoi(f[2].c_str());
            if (f.size() >= 4) FParsePresses(f[3], s.apress);  // (the CSV's presses meanwhile, e.g. shots until a hit's scene)
        }
        else if (f[0] == "intovol" && f.size() >= 2)
        {
            s.kind = STEPR::IntoVol; s.frames = 1;
            s.oid = std::atoi(f[1].c_str()); s.dz = f.size() >= 3 ? std::strtof(f[2].c_str(), nullptr) : 0.0f;
        }
        else if (f[0] == "tail" && f.size() >= 5)
        {
            // tail CID OID DIST N [DZ] [CSV] [until=TASK] [min=R] [3d] [lock] [trail=K]: for N frames (or until TASK is done), whenever the player is farther than DIST (2D) from the nearest such
            // object, he is put DIST/2 behind it (along its motion), DZ above its position (a follow job by teleports)
            s.kind = STEPR::Tail;
            // (until=TASK anywhere after N: the step also ends once that task is done (state >= 2))
            for (size_t i = 5; i < f.size(); ++i)
                if (f[i].rfind("until=", 0) == 0) { s.oidUntil = std::atoi(f[i].c_str() + 6); f.erase(f.begin() + long(i--)); }
                else if (f[i].rfind("min=", 0) == 0) { s.within = std::strtof(f[i].c_str() + 4, nullptr); f.erase(f.begin() + long(i--)); }
                else if (f[i] == "3d") { s.f3d = true; f.erase(f.begin() + long(i--)); }
                else if (f[i] == "lock") { s.fLock = true; f.erase(f.begin() + long(i--)); }
                else if (f[i].rfind("trail=", 0) == 0) { s.cTrail = std::atoi(f[i].c_str() + 6); s.fLock = true; f.erase(f.begin() + long(i--)); }
            s.cid = static_cast<int>(std::strtol(f[1].c_str(), nullptr, 0)); s.oid = std::atoi(f[2].c_str());
            s.near = std::strtof(f[3].c_str(), nullptr); s.frames = std::atoi(f[4].c_str());
            s.dz = f.size() >= 6 ? std::strtof(f[5].c_str(), nullptr) : 50.0f;
            if (f.size() >= 7) FParsePresses(f[6], s.apress);  // (repeated every 60 frames, e.g. CIRCLE for a pickpocket)
        }
        else if (f[0] == "pocket" && f.size() >= 3)
        {
            // pocket OID N [DZ] [CSV] [until=TASK]: for N frames (or until TASK is done) the player kept in guard OID's
            // pickpocket volume (its +0x3F4, option 304, while it is in the world: its pocket glows), the CSV's presses
            // every 60 frames (CIRCLE); the pickpocket's own conditions (target.cpp rank 3: the player inside that
            // shape, the guard not alerted; sly.cpp: the pocket's mode 2) decide the catch
            s.kind = STEPR::Tail; s.fPocket = true; s.cid = 0x3A;
            for (size_t i = 3; i < f.size(); ++i)
                if (f[i].rfind("until=", 0) == 0) { s.oidUntil = std::atoi(f[i].c_str() + 6); f.erase(f.begin() + long(i--)); }
            s.oid = std::atoi(f[1].c_str()); s.frames = std::atoi(f[2].c_str());
            s.dz = f.size() >= 4 ? std::strtof(f[3].c_str(), nullptr) : 20.0f;
            if (f.size() >= 5) FParsePresses(f[4], s.apress);
        }
        else if (f[0] == "onlo" && f.size() >= 2)
        {
            // tail CID OID DIST N [DZ] [CSV]: N frames (the CSV's presses every 60 frames), the player put DIST/2 behind the nearest such object whenever farther than DIST; onlo OID [DZ] [N] [DX DY [H]]: the player onto the N-th in-world copy of OID (DZ above its position), with its velocity
            s.kind = STEPR::OnLo; s.frames = 2;
            s.oid = std::atoi(f[1].c_str()); s.dz = f.size() >= 3 ? std::strtof(f[2].c_str(), nullptr) : 0.0f;
            s.cid = f.size() >= 4 ? std::atoi(f[3].c_str()) : 0;
            // Optional world-space offset and heading retain the platform's velocity
            // while placing the player on a particular part of a moving object.
            if (f.size() >= 6) { s.x = std::strtof(f[4].c_str(), nullptr); s.y = std::strtof(f[5].c_str(), nullptr); }
            if (f.size() >= 7) { s.h = std::strtof(f[6].c_str(), nullptr); s.fH = true; }
        }
        else if (f[0] == "giver" && f.size() >= 2)
        {
            s.kind = STEPR::Giver; s.frames = 1;
            s.oid = std::atoi(f[1].c_str()); s.dz = f.size() >= 3 ? std::strtof(f[2].c_str(), nullptr) : 150.0f;
        }
        else if (f[0] == "waitclock" && f.size() >= 3)
        {
            s.kind = STEPR::WaitClock;
            s.x = std::strtof(f[1].c_str(), nullptr); s.frames = std::atoi(f[2].c_str());
        }
        else if (f[0] == "taskreq" && f.size() >= 3)
        {
            s.kind = STEPR::TaskReq; s.frames = 1;
            s.oid = std::atoi(f[1].c_str()); s.cid = std::atoi(f[2].c_str());
        }
        else if (f[0] == "taskforce" && f.size() >= 3)
        {
            // taskforce OID N (Sly 3, native only): ForceTaskState (orig3 0x1890B0's request, no rules: the state with what
            // it implies), e.g. three jobs closed so that a chalk talk becomes available during play
            s.kind = STEPR::TaskReq; s.frames = 1; s.fH = true;
            s.oid = std::atoi(f[1].c_str()); s.cid = std::atoi(f[2].c_str());
        }
        else if (f[0] == "warpworld" && f.size() >= 2)
        {
            // warpworld N: the world record N loaded in this process (the debug menu's warp, RequestWorldWarp), its default
            // point; the route goes on through the load (e.g. tools/spintripregress.py's trip). warpworld N:TASK (Sly 3): the
            // world loaded at that task's start (RequestTaskStart, as the episode chooser 0x289198 starts the Intro at
            // 35:1798), e.g. m_ext loaded a second time in the process (tools/dial3.sh's code panel case)
            s.kind = STEPR::WarpWorld; s.frames = 1;
            s.oid = std::atoi(f[1].c_str());
            s.cid = f[1].find(':') != std::string::npos ? std::atoi(f[1].c_str() + f[1].find(':') + 1) : -1;
        }
        else if (f[0] == "startscpt" && f.size() >= 2)
        {
            // startscpt OID (Sly 3, native only): the script started as its owner's use would start it (the player as
            // the starter), e.g. i_trainer's door script 163581 (exit 4570 to the episode chooser)
            s.kind = STEPR::StartScpt; s.frames = 1;
            s.oid = std::atoi(f[1].c_str());
        }
        else if (f[0] == "wear" && f.size() >= 2)
        {
            // wear ID (Sly 3, native only): the current player puts the outfit on as its disguise gadget does
            // (gadget.cpp: 0x75A China's disguise), -1 takes it off
            s.kind = STEPR::Wear; s.frames = 1;
            s.oid = std::atoi(f[1].c_str());
        }
        else if (f[0] == "gadget" && f.size() >= 2)
        {
            // gadget ID (Sly 3, native only): the current player starts its Sly 3 item ID as its button does (vt slot 100,
            // gadget.cpp; e.g. 36 the Treasure Map)
            // gadget ID SLOT: the item put into the ability slot SLOT first (0 L1, 1 L2, 2 R2: the gadget menu's assignment,
            // 0x141FC8), for a gadget held by its button (e.g. 7 Bentley's Fishing Pole on L1)
            s.kind = STEPR::Gadget; s.frames = 1;
            s.oid = std::atoi(f[1].c_str());
            s.cid = f.size() >= 3 ? std::atoi(f[2].c_str()) : -1;
        }
        else if (f[0] == "holdbtn" && f.size() >= 2)
        {
            // holdbtn NAME|off: the button kept down from now on (until holdbtn off), e.g. the Treasure Map's gadget
            // button, which Sly 3 needs held (orig3 0x1AC098 case 24)
            s.kind = STEPR::HoldBtn; s.frames = 1;
            std::vector<PRESSR> a;
            s.cid = f[1] != "off" && FParsePresses(f[1] + "@1", a) && !a.empty() ? a[0].grf : 0;
        }
        else if (f[0] == "password" && f.size() >= 2)
        {
            // password FRAMES (Sly 3): until a guard's password check is passed (and no other holds the player), at most
            // FRAMES frames: a check that waits for the answer gets the buttons it showed (aipassword3.cpp), one press
            // each 8 frames: the player answering right; from then on the route answers every check
            s.kind = STEPR::Password;
            s.frames = std::atoi(f[1].c_str());
        }
        else if (f[0] == "photo" && f.size() >= 2)
        {
            // photo on|off (Sly 3): R3 (the camera / binocucom) pressed only when its view is not already so, e.g. a
            // job's script that opened the camera itself (Ep4's take_picture_3) or a photo that leaves it up
            s.kind = STEPR::Photo; s.frames = 600;   // (ends 30 frames after the check; at most 600 frames)
            s.cid = f[1] == "on" ? 1 : 0;
        }
        else if (f[0] == "skipif" && f.size() >= 4)
        {
            s.kind = STEPR::SkipIf; s.frames = 1;
            s.oid = std::atoi(f[1].c_str()); s.cid = std::atoi(f[2].c_str()); s.jump = std::atoi(f[3].c_str());
        }
        // skipifpo OID N: the next N steps skipped when the current player object is OID (e.g. the Guru riding guard
        // 119107: the guard has his pad; the remaining leap attempts are left out)
        else if (f[0] == "skipifpo" && f.size() >= 3)
        {
            s.kind = STEPR::SkipIf; s.frames = 1;
            s.oid = std::atoi(f[1].c_str()); s.cid = -1000; s.jump = std::atoi(f[2].c_str());
        }
        else if (f[0] == "waittask" && f.size() >= 4)
        {
            s.kind = STEPR::WaitTask;
            s.oid = std::atoi(f[1].c_str()); s.cid = std::atoi(f[2].c_str()); s.frames = std::atoi(f[3].c_str());
            if (f.size() >= 5) FParsePresses(f[4], s.apress);
        }
        else { std::fprintf(stderr, "route: bad line: %s\n", line.c_str()); continue; }
        for (size_t i = 1; i < f.size(); ++i)
        {
            if (f[i].rfind("fire=@", 0) == 0)
            {
                s.fFire = s.fFirePoint = true;
                std::sscanf(f[i].c_str() + 6, "%f:%f", &s.xFire, &s.yFire);
            }
            else if (f[i].rfind("fire=", 0) == 0)
            {
                s.fFire = true;
                for (const char* q = f[i].c_str() + 5; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr) s.aoidFire.push_back(std::atoi(q));
            }
            else if (f[i].rfind("firerange=", 0) == 0) s.rFire = std::strtof(f[i].c_str() + 10, nullptr);
            else if (f[i] == "fireprio") s.fFirePrio = true;
            else if (f[i].rfind("firenear=", 0) == 0) s.rFireNear = std::strtof(f[i].c_str() + 9, nullptr);
            else if (f[i] == "untilclear") s.fUntilClear = true;
            else if (f[i].rfind("yaw=", 0) == 0) std::sscanf(f[i].c_str() + 4, "%d:%f:%f", &s.oidYaw, &s.aYaw0, &s.aYaw1);
            else if (f[i].rfind("hold=", 0) == 0)
            {
                s.fHold = true;
                std::sscanf(f[i].c_str() + 5, "%f:%f", &s.xHold, &s.yHold);
            }
        }
        r.astep.push_back(s);
    }
    std::printf("route: %s, %zu steps, epoch %s%g\n", path.c_str(), r.astep.size(), r.tEpoch >= 0 ? "t " : "frame ",
                r.tEpoch >= 0 ? r.tEpoch : float(r.frameEpoch));
    if (const char* szT = std::getenv("SLY2N_ROUTETRACE"))
        if ((r.pfTrace = std::fopen(szT, "w"))) std::fprintf(r.pfTrace, "frame,t,step,buttons,lx,ly,x,y,z,pan\n");
}

// the reference's stick_axis (Python round: nearest, ties to even)
uint8_t BStick(double dir, double strength)
{
    const double v = strength == 1 ? 128 - 127 * dir : 128 - 127 * (dir * strength);
    const double lo = std::floor(v), fr = v - lo;
    long n = static_cast<long>(lo);
    if (fr > 0.5 || (fr == 0.5 && (n & 1))) ++n;
    return static_cast<uint8_t>(std::clamp(n, 0L, 255L));
}

// the left stick's bytes that ReadJoyPs2 turns into the pad's (x, y) (x right, y up; joy.cpp GetJoyXYDeflection: the
// direction kept, the deflection the largest of the four axes' readings over the limits seen so far, less 0.35, doubled)
void StickBytesFor(float x, float y, uint8_t* pbx, uint8_t* pby)
{
    const float l = std::sqrt(x * x + y * y);
    if (l < 1e-3f) { *pbx = *pby = 0x80; return; }
    const float u = std::min(1.0f, l), dx = x / l, dy = y / l;
    static const float s_aaxis[4][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {0x1.666666p-1f, 0x1.666666p-1f}, {0x1.666666p-1f, -0x1.666666p-1f}};
    float rMax = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        const float d = dx * s_aaxis[i][0] + dy * s_aaxis[i][1];
        const float den = d >= 0.0f ? g_joyps2.almDeflect[i].gMax : g_joyps2.almDeflect[i].gMin;
        if (den != 0.0f) rMax = std::max(rMax, d / den);
    }
    const float k = rMax > 0.0f ? (u * 0.5f + 0.35f) / rMax : 0.0f;
    const auto byte = [](float v) { return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L)); };
    *pbx = byte((std::clamp(k * dx, -1.0f, 1.0f) + 1.0f) * 127.5f);
    *pby = byte((1.0f - std::clamp(k * dy, -1.0f, 1.0f)) * 127.5f);
}

// the in-world object of class cid / oid nearest (x, y) (2D), as the reference walks SW +0x3038's class list
// (chase, pzr given (the step's ZMIN / ZMAX): only targets in that height window, and a guard (class 0x38..0x43) whose
// health (+0x33C) is gone is skipped)
bool FNearestObj(int cid, int oid, float x, float y, glm::vec3* ppos, const float* pzr = nullptr, LO** pplo = nullptr, int cidParent = -1, bool fMaxHp = false)
{
    float dBest = 1e30f;
    int hpBest = -1;
    if (!g_psw) return false;
    for (const auto& up : g_psw->aploAll)
    {
        LO* plo = up.get();
        static const bool s_fDbg = std::getenv("SLY2N_CHASEDBG") != nullptr;  // (debug: why a chase target is skipped)
        if (s_fDbg && plo->oid == oid)
            std::printf("chase: oid %d cid 0x%X (want 0x%X) in world %d hp %d z %.0f window %.0f..%.0f\n", oid, plo->Cid(), cid,
                        plo->fInWorld, PguardFromLo(plo) ? PguardFromLo(plo)->n33C : -999,
                        FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(plo)).z : 0.0f, pzr ? pzr[0] : 0.0f, pzr ? pzr[1] : 0.0f);
        // (oid -1: any object of the class, -OID: any but OID; their guards only while they have health)
        if (oid < 0 && !(plo->Cid() == cid && plo->fInWorld && plo->oid != -oid && (!PguardFromLo(plo) || PguardFromLo(plo)->n33C > 0))) continue;
        if ((oid >= 0 && plo->oid != oid && plo->oidInstance != oid) || !plo->fInWorld || plo->Cid() != cid || !FCidDerivedFrom(plo->Cid(), S3CID(0x30))) continue;  // (an instance by its alias too)
        if (pzr && cid >= 0x38 && cid < 0x44)
            if (const GUARD* pg = PguardFromLo(plo); pg && pg->n33C <= 0) continue;
        if (cidParent >= 0 && !(static_cast<ALO*>(plo)->paloParent && static_cast<ALO*>(plo)->paloParent->Cid() == cidParent)) continue;
        const glm::vec3 p = PosAloCenter(static_cast<ALO*>(plo));
        if (pzr && (p.z < pzr[0] || p.z > pzr[1])) continue;
        const float d = std::hypot(p.x - x, p.y - y);
        const int hp = fMaxHp && PguardFromLo(plo) && PguardFromLo(plo)->n33C > 0 ? PguardFromLo(plo)->n338 : 0;   // (its full health: the boss stays the target as he is hit)
        if (fMaxHp && hp != hpBest ? hp > hpBest : d < dBest) { dBest = d; hpBest = hp; *ppos = p; if (pplo) *pplo = plo; }
    }
    return dBest < 1e30f;
}
}  // namespace

// (joy.cpp's UpdateJoy: the program's pad sample of this frame, replacing the scripted presses once it runs)
bool FRouteSample(int frame, uint16_t* pgrf, uint8_t abAxis[4])
{
    ROUTE& r = Route();
    if (!r.fLoaded) Load();
    static const bool s_fHp = std::getenv("SLY2N_HPTRACE") != nullptr;  // (debug: the player's health changes)
    // (test only: SLY2N_HOLDHP=N: the current player's health held at least N each frame, a long job route's hits
    // survived; as the reference's SLY2_POKE on the actor's counter)
    if (static const char* s_szHold = std::getenv("SLY2N_HOLDHP"); s_szHold && g_psw)
        if (LO* ploH = PloPoCurrent(g_psw))
            if (ACTOR* pa = PactorFromLo(ploH))
                if (int* pn = const_cast<int*>(DmgCounterOf(pa)); pn && *pn > 0 && *pn < std::atoi(s_szHold)) *pn = std::atoi(s_szHold);
    // (and a hack ship's hit points, hack3.cpp: the bot's hack routes against Sly 3's enemy hit state 4 (AUDIT4 MODES-04))
    if (static const char* s_szHold = std::getenv("SLY2N_HOLDHP"); s_szHold && g_psw)
        if (LO* ploH = PloPoCurrent(g_psw); ploH && !PactorFromLo(ploH))
            if (int n = 0; FHackHp3(ploH, false, &n) && n > 0 && n < std::atoi(s_szHold)) FSetHackHp3(ploH, false, std::atoi(s_szHold));
    if (s_fHp)
    {
        static int s_hp = -1000;
        LO* ploH = g_psw ? PloPoCurrent(g_psw) : nullptr;
        ACTOR* pa = ploH ? PactorFromLo(ploH) : nullptr;
        const int* pn = pa ? DmgCounterOf(pa) : nullptr;
        if (pn && *pn != s_hp)
        {
            const glm::vec3 p = FCidDerivedFrom(ploH->Cid(), S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(ploH)) : glm::vec3(0.0f);
            std::printf("hp: frame %d t %.3f %d -> %d at (%.0f %.0f %.0f)\n", frame, g_clock.t, s_hp, *pn, p.x, p.y, p.z);
            s_hp = *pn;
        }
    }
    // (debug: SLY2N_GUARDHP=oid[,oid..]: those guards' health +0x33C changes, e.g. the Ep3 boss Rajan 95460)
    if (static const char* s_szGhp = std::getenv("SLY2N_GUARDHP"); s_szGhp && g_psw)
    {
        static std::unordered_map<int, int> s_mphp;
        for (const char* q = s_szGhp; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
        {
            const int oid = std::atoi(q);
            LO* plo = PloFindSwObjectByOid(g_psw, oid);
            const GUARD* pg = plo ? PguardFromLo(plo) : nullptr;
            if (!pg) continue;
            auto it = s_mphp.find(oid);
            if (it == s_mphp.end() || it->second != pg->n33C)
            {
                std::printf("guardhp: frame %d t %.3f %d %d -> %d\n", frame, g_clock.t, oid, it == s_mphp.end() ? -1000 : it->second, pg->n33C);
                s_mphp[oid] = pg->n33C;
            }
        }
    }
    // (test only: SLY2N_GUARDHPMAX=N: every guard's health +0x33C held at most N, a route's fights shortened)
    if (static const char* s_szMax = std::getenv("SLY2N_GUARDHPMAX"); s_szMax && g_psw)
        for (const auto& up : g_psw->aploAll)
            if (GUARD* pg = FCidDerivedFrom(up->Cid(), S3CID(0x38)) ? PguardFromLo(up.get()) : nullptr)
                pg->n33C = std::min(pg->n33C, std::atoi(s_szMax));
    if (r.astep.empty() || r.fDone || r.fFailed) return false;
    if (r.frameStart < 0)
    {
        const bool fEpoch = r.tEpoch >= 0 ? g_clock.t >= r.tEpoch : frame >= r.frameEpoch;
        if (!fEpoch) return false;
        r.frameStart = frame + r.after;
        std::printf("route: epoch at frame %d (t %.3f), start at %d\n", frame, g_clock.t, r.frameStart);
    }
    if (frame < r.frameStart) return false;
    uint16_t grf = 0;
    uint8_t a[4] = {0x80, 0x80, 0x80, 0x80};
    if (!r.fStarted) { r.fStarted = true; r.frameStep = frame; std::printf("route: step 1 at frame %d (t %.3f)\n", frame, g_clock.t); }
    const STEPR* s = &r.astep[r.istep];
    const auto fOpen = [](const STEPR* p) { return p->kind == STEPR::Move || p->kind == STEPR::Chase || p->kind == STEPR::WaitObj || p->kind == STEPR::WaitTask || p->kind == STEPR::WaitClock || p->kind == STEPR::WaitScpt || p->kind == STEPR::WaitCmd || p->kind == STEPR::WaitPo || p->kind == STEPR::DrivePath || p->kind == STEPR::Dogfight || p->kind == STEPR::Row || p->kind == STEPR::Sail || p->kind == STEPR::Broadside ||
                                           (p->kind == STEPR::Point && p->oidUntil >= 0); };
    if (r.fAdvance || (!fOpen(s) && frame - r.frameStep >= s->frames))
    {
        ++r.istep;
        r.fAdvance = false;
        r.frameStep = frame;
        if (r.istep == static_cast<int>(r.astep.size()))
        {
            r.fDone = true;
            std::printf("route: completed at frame %d (t %.3f)\n", frame, g_clock.t);
            return false;
        }
        s = &r.astep[r.istep];
        std::printf("route: step %d at frame %d (t %.3f)\n", r.istep + 1, frame, g_clock.t);
    }
    const int rel = frame - r.frameStep + 1;
    LO* plo = g_psw ? PloPoCurrent(g_psw) : nullptr;
    glm::vec3 pos(0.0f);
    if (plo && FCidDerivedFrom(plo->Cid(), S3CID(0x30))) pos = PosAloCenter(static_cast<ALO*>(plo));
    const float camx = g_cm.mat[0].x, camy = g_cm.mat[0].y;
    if (s->kind == STEPR::WaitObj)
    {
        glm::vec3 po;
        if (FNearestObj(s->cid, s->oid, s->x, s->y, &po) && std::hypot(po.x - s->x, po.y - s->y) <= s->near)
        {
            std::printf("route: step %d's object at %.0f %.0f %.0f (frame %d)\n", r.istep + 1, po.x, po.y, po.z, frame);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; std::printf("route: completed at frame %d\n", frame); }
            else r.fAdvance = true;
        }
        else if (frame - r.frameStep >= s->frames)
        {
            std::printf("route: failed at frame %d: step %d's object never came\n", frame, r.istep + 1);
            r.fFailed = true;
            return false;
        }
    }
    if (s->kind == STEPR::Teleport && rel == 1)
    {
        g_routetele = {true, s->x, s->y, s->z, s->h, s->fH};
        std::printf("route: step %d teleport %.0f %.0f %.0f (frame %d)\n", r.istep + 1, s->x, s->y, s->z, frame);
    }
    if (s->kind == STEPR::HackDock && rel == 1)
    {
        g_routehackdock = {true, s->x, s->y};
        std::printf("route: step %d hackdock %.0f %.0f (frame %d)\n", r.istep + 1, s->x, s->y, frame);
    }
    if (s->kind == STEPR::SetPos && rel == 1 && plo && FCidDerivedFrom(plo->Cid(), S3CID(0x30)))
    {
        // as SLY2N_SETPOS / the reference's route setpos (SLY2_SETPOS): the class's slot 29, velocities zeroed
        ALO* palo = static_cast<ALO*>(plo);
        if (auto pfn = reinterpret_cast<void (*)(ALO*, const glm::vec3&)>(PvNativeVt(plo, 29))) pfn(palo, glm::vec3(s->x, s->y, s->z));
        else SetAloPos(palo, glm::vec3(s->x, s->y, s->z));
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        std::printf("route: step %d setpos %.1f %.1f %.1f (frame %d)\n", r.istep + 1, s->x, s->y, s->z, frame);
    }
    if (s->kind == STEPR::SetPosLo && rel == 1 && plo && FCidDerivedFrom(plo->Cid(), S3CID(0x30)) && g_psw)
    {
        float dBest = s->near;
        glm::vec3 pBest(0.0f);
        int oidBest = -1;
        for (const auto& up : g_psw->aploAll)
        {
            LO* pl = up.get();
            if (!pl->fInWorld || !FCidDerivedFrom(pl->Cid(), S3CID(0x30)) || std::find(s->aoid.begin(), s->aoid.end(), pl->oid) == s->aoid.end()) continue;
            if (pl->paloParent) continue;  // (loose ones only: not the one carried (parented to its holder))
            const glm::vec3 p = PosAloCenter(static_cast<ALO*>(pl));
            if (p.z < s->zmin || p.z > s->zmax) continue;
            const float d = std::hypot(p.x - s->x, p.y - s->y);
            if (d < dBest) { dBest = d; pBest = p; oidBest = pl->oid; }
        }
        if (oidBest >= 0)
        {
            ALO* palo = static_cast<ALO*>(plo);
            glm::vec3 q = pBest + glm::vec3(0.0f, 0.0f, s->dz);
            if (const float d = std::hypot(s->x - pBest.x, s->y - pBest.y); s->h > 0.0f && d > 1.0f)
                q += glm::vec3((s->x - pBest.x) / d * s->h, (s->y - pBest.y) / d * s->h, 0.0f);
            if (auto pfn = reinterpret_cast<void (*)(ALO*, const glm::vec3&)>(PvNativeVt(plo, 29))) pfn(palo, q);
            else SetAloPos(palo, q);
            SetAloVelocityTo(palo, glm::vec3(0.0f));
            SetAloAngVelocityTo(palo, glm::vec3(0.0f));
            std::printf("route: step %d setposlo %d at %.0f %.0f %.0f (frame %d)\n", r.istep + 1, oidBest, pBest.x, pBest.y, pBest.z, frame);
        }
        else std::printf("route: step %d setposlo: none within %.0f of %.0f %.0f (frame %d)\n", r.istep + 1, s->near, s->x, s->y, frame);
    }
    if (s->kind == STEPR::Point && s->oidUntil >= 0)
    {
        extern std::unordered_map<int, int> g_mpoidcScptStart;  // scpt.cpp
        static int s_c0 = 0;
        const int c = g_mpoidcScptStart[s->oidUntil];
        if (rel == 1) s_c0 = c;
        else if (c > s_c0 || frame - r.frameStep >= s->frames)
        {
            std::printf("route: step %d's script %d %s (frame %d t %.3f)\n", r.istep + 1, s->oidUntil, c > s_c0 ? "started" : "timeout", frame, g_clock.t);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    }
    if (s->kind == STEPR::WaitPo && ((plo && plo->oid == s->oid) || frame - r.frameStep >= s->frames))
    {
        std::printf("route: step %d's player %d %s (frame %d t %.3f)\n", r.istep + 1, s->oid, plo && plo->oid == s->oid ? "current" : "timeout", frame, g_clock.t);
        if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
        r.fAdvance = true;
    }
    if (s->kind == STEPR::DrivePath)
    {
        LO* ploCrv = g_psw ? PloFindSwObjectByOid(g_psw, s->oid) : nullptr;
        const CRV* pcrv = ploCrv ? PcrvFromLo(ploCrv) : nullptr;
        ALO* palo = plo && FCidDerivedFrom(plo->Cid(), S3CID(0x30)) ? static_cast<ALO*>(plo) : nullptr;
        static float s_s = 0.0f;
        if (rel == 1) s_s = 0.0f;
        bool fEnd = frame - r.frameStep >= s->frames;
        if (pcrv && palo && PxfObj(palo))
        {
            NearestCrvS(pcrv, pos, s_s, nullptr, nullptr, nullptr, &s_s);
            const float sMax = SLengthCrv(pcrv);
            glm::vec3 posEnd, posAim;
            EvalCrvS(pcrv, sMax, &posEnd, nullptr);
            EvalCrvS(pcrv, std::min(s_s + s->dz, sMax), &posAim, nullptr);
            const glm::vec3 local = glm::vec3(glm::inverse(PxfObj(palo)->matWorld) * glm::vec4(posAim, 1.0f));
            const double rad = std::atan2(local.y, local.x);  // (left of the heading: positive, as the car's steering)
            const double x = std::clamp(rad * 2.5, -1.0, 1.0);  // (BStick: positive = the stick left = the car turns left)
            a[0] = BStick(x, 1.0);
            static const float s_radCoast = std::getenv("SLY2N_DRIVECOAST") ? std::strtof(std::getenv("SLY2N_DRIVECOAST"), nullptr) : 0.5f;
            static glm::vec3 s_posLast(0.0f);
            const float sv = rel == 1 || g_clock.dt <= 0.0f ? 0.0f : glm::length(glm::vec2(pos - s_posLast)) / g_clock.dt;
            s_posLast = pos;
            // (off the aim by more than s_radCoast while faster than 500: X let go, the car coasts through the turn)
            if (std::fabs(rad) < s_radCoast || sv < 500.0f) grf |= BTN_Cross;
            if (static const int s_nPrint = std::getenv("SLY2N_DRIVEPRINT") ? std::max(1, std::atoi(std::getenv("SLY2N_DRIVEPRINT"))) : 0; s_nPrint && rel % s_nPrint == 1 % s_nPrint)
                std::printf("route: drive at %.0f %.0f %.0f s %.0f / %.0f aim %.0f %.0f err %.2f stick %.2f speed %.0f up %.2f (frame %d)\n", pos.x, pos.y, pos.z, s_s, sMax, posAim.x, posAim.y, rad, x, sv, PxfObj(palo)->matWorld[2].z, frame);
            // (debug: SLY2N_DRIVENEAR=R: on a sudden stop (a hit), the in-world objects within R of the car)
            static float s_svLast = 0.0f;
            if (static const float s_rNear = std::getenv("SLY2N_DRIVENEAR") ? std::strtof(std::getenv("SLY2N_DRIVENEAR"), nullptr) : 0.0f; s_rNear > 0.0f && s_svLast > 600.0f && sv < 0.5f * s_svLast)
                for (const auto& up : g_psw->aploAll)
                {
                    LO* pl = up.get();
                    if (pl == plo || !pl->fInWorld || !FCidDerivedFrom(pl->Cid(), S3CID(0x30))) continue;
                    const glm::vec3 q = PosAloCenter(static_cast<ALO*>(pl));
                    if (glm::length(q - pos) < s_rNear)
                        std::printf("route: drive hit near %d (cid 0x%X parent %d) at %.0f %.0f %.0f (frame %d)\n", pl->oid, pl->Cid(), pl->paloParent ? pl->paloParent->oid : -1, q.x, q.y, q.z, frame);
                }
            s_svLast = sv;
            if (glm::length(glm::vec2(posEnd - pos)) <= s->near) fEnd = true;
        }
        if (fEnd)
        {
            std::printf("route: step %d's drive along %d %s at %.0f %.0f %.0f (frame %d t %.3f)\n", r.istep + 1, s->oid, frame - r.frameStep >= s->frames ? "timeout" : "ended", pos.x, pos.y, pos.z, frame, g_clock.t);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    }
    if (s->kind == STEPR::Dogfight)
    {
        float gx = 0.0f, gy = 0.0f;
        bool fFire = false;
        int nSpeed = 0;
        if (plo && FDogfightStick3(plo, &gx, &gy, &fFire, &nSpeed))
        {
            StickBytesFor(gx, gy, &a[0], &a[1]);   // (the pad's x / y: through the stick's dead zone and limits)
            if (fFire) grf |= BTN_R1;
            if (fFire && rel % 8 < 2) grf |= BTN_L1;   // (the plane's weapon: a press every 8 frames)
            if (nSpeed > 0) grf |= BTN_Cross;          // the boost
            else if (nSpeed < 0) grf |= BTN_Square;    // the brake
        }
        const int nTask = g_psw ? NTaskStateOid(s->oid, nullptr) : -2;
        if ((nTask != -2 && nTask >= 2) || frame - r.frameStep >= s->frames)
        {
            std::printf("route: step %d's dogfight %s: task %d state %d (frame %d t %.3f)\n", r.istep + 1, nTask >= 2 ? "ended" : "timeout", s->oid, nTask, frame, g_clock.t);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    }
    if (s->kind == STEPR::Row)
    {
        float gx = 0.0f, gy = 0.0f;
        bool fStroke = false;
        if (plo && FRowStick3(plo, s->oid, &gx, &gy, &fStroke))
        {
            StickBytesFor(gx, gy, &a[0], &a[1]);
            if (fStroke && rel % 16 < 4) grf |= BTN_Cross;
        }
        const int nTask = g_psw ? NTaskStateOid(s->oid, nullptr) : -2;
        if ((nTask != -2 && nTask >= 2) || frame - r.frameStep >= s->frames)
        {
            std::printf("route: step %d's row %s: task %d state %d (frame %d t %.3f)\n", r.istep + 1, nTask >= 2 ? "ended" : "timeout", s->oid, nTask, frame, g_clock.t);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    }
    if (s->kind == STEPR::Sail)
    {
        unsigned grfSteer = 0;
        float dist = 3.4e38f;
        LO* ptarget = g_psw ? PloFindSwObjectByOid(g_psw, s->cid) : nullptr;
        const bool fSail = plo && ptarget && FSailToward3(plo, PosLoWorld(ptarget), &grfSteer, &dist);
        if (fSail) grf |= uint16_t(grfSteer << 8);
        if (rel == 1 || rel % 600 == 0)
            std::printf("route: sail %d (%s) from %d: ok %d distance %.0f buttons %u\n", s->cid, ptarget ? "found" : "missing", plo ? plo->oid : -1, int(fSail), dist, grfSteer);   // (the game's L2 / R2 0x1 / 0x2: BTN_L2 / BTN_R2)
        const int nTask = g_psw && s->oid > 0 ? NTaskStateOid(s->oid, nullptr) : -2;
        if (dist < s->near || (nTask != -2 && nTask >= 2) || frame - r.frameStep >= s->frames)
        {
            std::printf("route: step %d's sail to %d %s: distance %.0f task %d state %d (frame %d t %.3f)\n", r.istep + 1, s->cid,
                        dist < s->near ? "arrived" : (nTask >= 2 ? "ended" : "timeout"), dist, s->oid, nTask, frame, g_clock.t);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    }
    if (s->kind == STEPR::Broadside)
    {
        glm::vec3 pos(0.0f), vel(0.0f);
        float heading = 0.0f;
        unsigned grfSteer = 0;
        bool fUse = false;
        const bool fSteer = plo && FShipBroadside3(plo, &pos, &heading, &vel, &grfSteer, &fUse);
        if (fSteer) grf |= uint16_t(grfSteer << 8);
        const bool fOk = fSteer && fUse;
        const int k = rel % 360;
        if (fOk && k == 1)
        {
            g_routetele = {true, pos.x + vel.x * g_clock.dt, pos.y + vel.y * g_clock.dt, pos.z + vel.z * g_clock.dt, heading, true};
            g_routetele.fVel = true; g_routetele.vel = vel;
            std::printf("route: step %d broadside: at %.0f %.0f %.0f heading %.2f (frame %d)\n", r.istep + 1, pos.x, pos.y, pos.z, heading, frame);
        }
        if (fOk && k >= 4 && k < 10) grf |= BTN_Circle;
        const int nTask = g_psw ? NTaskStateOid(s->oid, nullptr) : -2;
        if ((nTask != -2 && nTask >= 2) || frame - r.frameStep >= s->frames)
        {
            std::printf("route: step %d's broadside %s: task %d state %d (frame %d t %.3f)\n", r.istep + 1, nTask >= 2 ? "ended" : "timeout", s->oid, nTask, frame, g_clock.t);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    }
    if (s->kind == STEPR::WaitCmd)
    {
        extern std::unordered_map<int64_t, int> g_mpcScptCmdExec3;  // game/scptgame3.cpp
        static int s_c0 = 0;
        const int c = g_mpcScptCmdExec3[(int64_t(s->oid) << 16) | s->cid];
        if (rel == 1) s_c0 = s->x != 0.0f ? 0 : c;
        else if (c > s_c0 || frame - r.frameStep >= s->frames)
        {
            std::printf("route: step %d's script %d command %d %s (frame %d t %.3f)\n", r.istep + 1, s->oid, s->cid, c > s_c0 ? "ran" : "timeout", frame, g_clock.t);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    }
    if (s->kind == STEPR::WaitScpt)
    {
        extern std::unordered_map<int, int> g_mpoidcScptStart;  // scpt.cpp
        static int s_c0 = 0;
        const int c = g_mpoidcScptStart[s->oid];
        if (rel == 1) s_c0 = c;
        else if (c > s_c0 || frame - r.frameStep >= s->frames)
        {
            std::printf("route: step %d's script %d %s (frame %d t %.3f)\n", r.istep + 1, s->oid, c > s_c0 ? "started" : "timeout", frame, g_clock.t);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    }
    if (s->kind == STEPR::WaitClock)
    {
        static float s_t0 = 0.0f;
        if (rel == 1) s_t0 = g_clock.t;
        if (g_clock.t - s_t0 >= s->x || frame - r.frameStep >= s->frames)
        {
            std::printf("route: step %d's clock ran %.3f (frame %d)\n", r.istep + 1, g_clock.t - s_t0, frame);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    }
    if (s->kind == STEPR::IntoVol && rel == 1 && g_psw)
    {
        // (the center of a volume's shape (a trigger or an exit) in the world, DZ added: the player teleported there)
        // (a clone's shape is its original's: the geometry from the clone chain, the matrix the clone's own, as
        // trigger.cpp FInsideVolume reads it; Sly 3's shared exits are such clones)
        const LOXSHAPE* px = dynamic_cast<const LOXSHAPE*>(PloFindSwObjectByOid(g_psw, s->oid));
        const LOXSHAPE* pgeom = px;
        for (const LO* p = px; pgeom && pgeom->geom.apos.empty() && p && p->ploCloneOf; p = p->ploCloneOf)
            if (const LOXSHAPE* q = dynamic_cast<const LOXSHAPE*>(p->ploCloneOf)) pgeom = q;
        if (px && pgeom && !pgeom->geom.apos.empty())
        {
            const glm::mat4 m = MatWorldLox(px);
            glm::vec3 mn(1e30f), mx(-1e30f);
            for (const glm::vec3& v : pgeom->geom.apos) { const glm::vec3 w = glm::vec3(m * glm::vec4(v, 1.0f)); mn = glm::min(mn, w); mx = glm::max(mx, w); }
            const glm::vec3 c = (mn + mx) * 0.5f;
            g_routetele = {true, c.x, c.y, mn.z + s->dz, 0.0f, false};
            std::printf("route: step %d into volume %d at %.0f %.0f %.0f (box %.0f..%.0f %.0f..%.0f %.0f..%.0f)\n", r.istep + 1, s->oid, c.x, c.y,
                        mn.z + s->dz, mn.x, mx.x, mn.y, mx.y, mn.z, mx.z);
        }
        else std::printf("route: step %d: no volume %d\n", r.istep + 1, s->oid);
    }
    if (s->kind == STEPR::Tail && g_psw && s->oidUntil >= 0 && rel > 1)
        if (const int nTask = NTaskStateOid(s->oidUntil, nullptr); nTask >= 2)
        {
            std::printf("route: step %d tail ended: task %d state %d (frame %d t %.3f)\n", r.istep + 1, s->oidUntil, nTask, frame, g_clock.t);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    if (s->kind == STEPR::Tail && s->fPocket && g_psw)
    {
        static int s_framePut = -1000;
        GUARD* pg = nullptr;
        for (const auto& up : g_psw->aploAll)
                if ((up->oid == s->oid || up->oidInstance == s->oid) && up->fInWorld && PguardFromLo(up.get())) { pg = PguardFromLo(up.get()); break; }
        LO* pv = pg ? pg->plo3F4 : nullptr;
        const LOXSHAPE* px = dynamic_cast<const LOXSHAPE*>(pv);
        const LOXSHAPE* pgeom = px;
        for (const LO* p = px; pgeom && pgeom->geom.apos.empty() && p && p->ploCloneOf; p = p->ploCloneOf)
            if (const LOXSHAPE* q = dynamic_cast<const LOXSHAPE*>(p->ploCloneOf)) pgeom = q;
        LO* pcur = PloPoCurrent(g_psw);
        SLY* psly = pcur ? PslyFromLo(pcur) : nullptr;
        const bool fPicking = psly && psly->jts == jts::Pickpocket;
        // (the step ends once a pickpocket took the pocket (its mode no longer 2 after Sly's state 11): the prize flies
        // out and the next steps pick it up)
        static bool s_fWasPicking = false;
        if (rel == 1) s_fWasPicking = false;
        if (fPicking) s_fWasPicking = true;
        if (s_fWasPicking && !fPicking && pg && PocketOf(pg->palo).nMode != 2)
        {
            std::printf("route: step %d pocket %d: taken (frame %d)\n", r.istep + 1, s->oid, frame);
            g_fRoutePocketPress = false;
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
            pv = nullptr;   // (no more puts)
        }
        if (s_fWasPicking && !fPicking) s_fWasPicking = false;
        // (CIRCLE as soon as the pickpocket can start (sly.cpp: the target of rank 3 is this guard, its pocket in mode
        // 2), 4 frames, at most every 12)
        static int s_framePress = -1000;
        const int jtsNow = psly ? int(psly->jts) : -1;
        const bool fGround = jtsNow == jts::Stand || jtsNow == jts::Run || jtsNow == jts::Walk || jtsNow == jts::Sneak;
        if (psly && pg && fGround && psly->n10E4 == 3 && psly->plo10DC == pg->palo && PocketOf(psly->plo10DC).nMode == 2 &&
            frame - s_framePress >= 12)
        {
            s_framePress = frame;
            std::printf("route: step %d pocket %d: CIRCLE (state %d, frame %d)\n", r.istep + 1, s->oid, jtsNow, frame);
        }
        g_fRoutePocketPress = frame - s_framePress < 4;
        if (pv && px && pv->fInWorld && pgeom && !pgeom->geom.apos.empty() && !fPicking && frame - s_framePut >= 15 && !FPointInsideShape(pv, pos))
        {
            const glm::mat4 m = MatWorldLox(px);
            glm::vec3 mn(1e30f), mx(-1e30f);
            for (const glm::vec3& v : pgeom->geom.apos) { const glm::vec3 w = glm::vec3(m * glm::vec4(v, 1.0f)); mn = glm::min(mn, w); mx = glm::max(mx, w); }
            const glm::vec3 c0 = (mn + mx) * 0.5f;
            const glm::vec3 pgd = PosAloCenter(pg->palo);
            // (the point of the shape nearest the pocket (on the line from the pocket to the shape's center): the arm
            // reaches only within 120 of its joint (pocket.cpp UpdatePickpocketPose), the shape's center is ~300 away)
            LO* ppocket = PloPocket(pg->palo);
            const glm::vec3 pk = ppocket ? PosLoWorld(ppocket) : pgd;
            glm::vec3 c = c0;
            for (int i = 0; i <= 20; ++i)
            {
                glm::vec3 q = pk + (c0 - pk) * (float(i) / 20.0f);
                q.z = c0.z;
                if (FPointInsideShape(pv, q)) { c = q; break; }
            }
            g_routetele = {true, c.x, c.y, mn.z + s->dz, std::atan2(pgd.y - c.y, pgd.x - c.x), true};
            s_framePut = frame;
            std::printf("route: step %d pocket %d: player put in volume %d at %.0f %.0f %.0f (frame %d)\n", r.istep + 1, s->oid, pv->oid, c.x, c.y, mn.z + s->dz, frame);
        }
    }
    if (s->kind == STEPR::Tail && !s->fPocket && g_psw)
    {
        static glm::vec3 s_posPrev(0.0f);
        static LO* s_ploLock = nullptr;
        glm::vec3 po;
        bool fFound;
        if (s->fLock)
        {
            if (rel == 1 || !s_ploLock || !s_ploLock->fInWorld)
            {
                s_ploLock = nullptr;
                fFound = FNearestObj(s->cid, s->oid, pos.x, pos.y, &po, nullptr, &s_ploLock);
            }
            else
            {
                po = PosAloCenter(static_cast<ALO*>(s_ploLock));
                fFound = true;
            }
        }
        else fFound = FNearestObj(s->cid, s->oid, pos.x, pos.y, &po);
        if (fFound)
        {
            if (rel == 1) s_posPrev = po;
            glm::vec3 d = po - s_posPrev;
            d.z = 0.0f;
            // (min=R: also when nearer than R while it moves (a guard walking back past the player would see him))
            const float dd = s->f3d ? glm::length(po - glm::vec3(pos.x, pos.y, pos.z)) : std::hypot(po.x - pos.x, po.y - pos.y);
            // (trail: the locked object's origin each frame, the oldest K frames back used)
            static std::deque<glm::vec3> s_aposTrail;
            if (s->cTrail > 0)
            {
                if (rel == 1) s_aposTrail.clear();
                // (only where it stands: not mid-jump, so the player is never put over a gap it leapt)
                if (s_ploLock && std::fabs(VelAloPos(static_cast<ALO*>(s_ploLock)).z) < 30.0f) s_aposTrail.push_back(PosLoWorld(s_ploLock));
                while (int(s_aposTrail.size()) > s->cTrail) s_aposTrail.pop_front();
            }
            if (rel > 1 && (dd > s->near || (s->within > 0.0f && dd < s->within && glm::length(d) > 1.0f)) &&
                (s->cTrail == 0 || int(s_aposTrail.size()) >= s->cTrail))
            {
                const glm::vec3 back = glm::length(d) > 1e-3f ? -glm::normalize(d) : glm::vec3(0.0f);
                const glm::vec3 q = s->cTrail > 0 ? s_aposTrail.front() + glm::vec3(0.0f, 0.0f, s->dz)
                                                  : po + back * (s->near * 0.5f) + glm::vec3(0.0f, 0.0f, s->dz);
                g_routetele = {true, q.x, q.y, q.z, 0.0f, false};
                std::printf("route: step %d tail %d: player put at %.0f %.0f %.0f (frame %d)\n", r.istep + 1, s->oid, q.x, q.y, q.z, frame);
            }
            if (rel % 15 == 0) s_posPrev = po;
        }
    }
    if (s->kind == STEPR::OnLo && g_psw)
    {
        // (frame 1: the copy's position; frame 2: the player put there, DZ above, moving with it)
        static glm::vec3 s_pos0(0.0f);
        int i = 0;
        for (const auto& up : g_psw->aploAll)
        {
            LO* pl = up.get();
            if (pl->oid != s->oid || !pl->fInWorld || !FCidDerivedFrom(pl->Cid(), S3CID(0x30)) || i++ != s->cid) continue;
            const glm::vec3 p = PosAloCenter(static_cast<ALO*>(pl));
            if (rel == 1) s_pos0 = p;
            else if (rel == 2)
            {
                const glm::vec3 v = g_clock.dt > 0.0f ? (p - s_pos0) / g_clock.dt : glm::vec3(0.0f);
                g_routetele = {true, p.x + v.x * g_clock.dt + s->x, p.y + v.y * g_clock.dt + s->y,
                              p.z + v.z * g_clock.dt + s->dz, s->h, s->fH};
                g_routetele.fVel = true; g_routetele.vel = v;
                std::printf("route: step %d onto %d#%d at %.0f %.0f %.0f vel %.0f %.0f %.0f (frame %d)\n", r.istep + 1, s->oid, s->cid, p.x, p.y,
                            p.z + s->dz, v.x, v.y, v.z, frame);
            }
            break;
        }
    }
    if (s->kind == STEPR::NoJt)
    {
        for (int k = 0; k < g_jttrack.c; ++k)
            if (const LO* pt = g_jttrack.ae[k].plo; pt && std::find(s->aoid.begin(), s->aoid.end(), pt->oid) != s->aoid.end())
            {
                std::printf("route: failed at frame %d: step %d's %d is an interaction target (%.0f away)\n", frame, r.istep + 1, pt->oid,
                            g_jttrack.ae[k].dist);
                r.fFailed = true;
                return false;
            }
    }
    if (s->kind == STEPR::Giver && rel == 1)
    {
        g_routetele.fGiver = true; g_routetele.oidGiver = s->oid; g_routetele.dGiver = s->dz;
        std::printf("route: step %d giver %d (frame %d)\n", r.istep + 1, s->oid, frame);
    }
    if (s->kind == STEPR::TaskReq && rel == 1)
    {
        bool FRequestTaskStateOid(OID oid, int n);  // (task.cpp: as SLY2N_TASKREQ)
        if (s->fH)
        {
            TASK3* p3 = PtaskFind3(s->oid);
            std::printf("route: step %d taskforce %d = %d (frame %d): %d\n", r.istep + 1, s->oid, s->cid, frame, p3 ? 1 : 0);
            if (p3) QueueTaskRequest3(1, p3, s->cid, p3->nMode, nullptr);
        }
        else
        {
            std::printf("route: step %d taskreq %d = %d (frame %d)\n", r.istep + 1, s->oid, s->cid, frame);
            std::printf("route: taskreq %d: %d\n", s->oid, FRequestTaskStateOid(s->oid, s->cid) ? 1 : 0);
        }
    }
    if (s->kind == STEPR::WarpWorld && rel == 1 && g_psw)
    {
        std::printf("route: step %d warpworld %d:%d (frame %d)\n", r.istep + 1, s->oid, s->cid, frame);
        if (s->cid >= 0) RequestTaskStart(g_psw, s->oid, s->cid);   // (transition.h: as the episode chooser)
        else RequestWorldWarp(g_psw, s->oid);  // (transition.h: as the debug menu)
    }
    // (the task's state through NTaskStateOid (task.h): Sly 3's tasks (game/task3) when the scripts run, else the
    // copied TASK objects; -2: no such task)
    if (s->kind == STEPR::StartScpt && rel == 1 && g_psw)
    {
        LO* pscpt = PloFindSwObjectByOid(g_psw, s->oid);
        std::printf("route: step %d startscpt %d%s (frame %d)\n", r.istep + 1, s->oid, pscpt ? "" : ": no such object", frame);
        if (pscpt) StartScptOfLo3(pscpt, PloPoCurrent(g_psw));
        r.fAdvance = true;
    }
    if (s->kind == STEPR::HoldBtn && rel == 1)
    {
        r.grfHeld = static_cast<uint16_t>(s->cid);
        std::printf("route: step %d holds buttons 0x%X (frame %d)\n", r.istep + 1, s->cid, frame);
        r.fAdvance = true;
    }
    if (s->kind == STEPR::Wear && rel == 1 && g_psw)
    {
        LO* plo = PloPoCurrent(g_psw);
        std::printf("route: step %d wear %d (frame %d): %d\n", r.istep + 1, s->oid, frame, plo && FWearOutfit3(plo, s->oid) ? 1 : 0);
        r.fAdvance = true;
    }
    if (s->kind == STEPR::Gadget && rel == 1 && g_psw)
    {
        LO* plo = PloPoCurrent(g_psw);
        ACTOR* pa = plo ? PactorFromLo(plo) : nullptr;
        std::printf("route: step %d gadget %d (frame %d): %s\n", r.istep + 1, s->oid, frame, pa ? "started" : "no actor");
        if (pa && s->cid >= 0 && s->cid < 3) SetActorAbilitySlot(pa, s->cid, sly3::Item2FromItem3(s->oid));
        if (pa) StartActorAbility(pa, sly3::Item2FromItem3(s->oid));
        r.fAdvance = true;
    }
    if (s->kind == STEPR::SkipIf && rel == 1)
    {
        const bool fPo = s->cid == -1000;
        LO* ploCur = fPo && g_psw ? PloPoCurrent(g_psw) : nullptr;
        const int nTask = fPo ? (ploCur && ploCur->oid == s->oid ? 0 : -2) : g_psw ? NTaskStateOid(s->oid, nullptr) : -2;
        if (nTask != -2 && nTask >= (fPo ? 0 : s->cid))
        {
            const int c = std::min(s->jump, static_cast<int>(r.astep.size()) - 1 - r.istep);
            std::printf("route: step %d's task %d state %d: %d steps skipped (frame %d)\n", r.istep + 1, s->oid, nTask, c, frame);
            r.istep += c;
        }
        r.fAdvance = true;
    }
    if (s->kind == STEPR::WaitTask)
    {
        const int nTask = g_psw ? NTaskStateOid(s->oid, nullptr) : -2;
        // (not while a world change is under way: a task reached in the world being left (e.g. v_interpol's chase
        // tasks after the HQ's exit fired) is the next world's to play, and a request there would land in the old one)
        if (nTask != -2 && nTask >= s->cid && !g_transition.fPending && g_transition.wipes != WIPES_Out)
        {
            std::printf("route: step %d's task %d state %d (frame %d t %.3f)\n", r.istep + 1, s->oid, nTask, frame, g_clock.t);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; std::printf("route: completed at frame %d\n", frame); }
            else r.fAdvance = true;
        }
        else if (frame - r.frameStep >= s->frames)
        {
            // (a timeout goes on: the next steps may retry, e.g. a moving photo target from another place)
            std::printf("route: timeout at frame %d: step %d's task %d at state %d\n", frame, r.istep + 1, s->oid, nTask == -2 ? -9 : nTask);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    }
    STEPR sChase;
    if (s->kind == STEPR::Chase)
    {
        glm::vec3 po;
        LO* ploChased = nullptr;
        const float azr[2] = {s->zmin, s->zmax};
        if (!FNearestObj(s->cid, s->oid, pos.x, pos.y, &po, azr, &ploChased, s->behind > 0.0f ? S3CID(0x49) : -1, s->maxhp) ||
            (s->within > 0.0f && std::hypot(po.x - pos.x, po.y - pos.y) > s->within))
        {
            // (none left in the height window: the chase is over, e.g. every guard of a fight knocked out)
            std::printf("route: step %d's object is gone (frame %d)\n", r.istep + 1, frame);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; std::printf("route: completed at frame %d\n", frame); }
            else r.fAdvance = true;
            return false;
        }
        if (static const bool s_fPrint = std::getenv("SLY2N_CHASEPRINT") != nullptr; s_fPrint && frame % 60 == 0)  // (debug: the chased object's distance)
            std::printf("route: chase %d at %.0f %.0f %.0f, Sly %.0f %.0f %.0f: %.0f (frame %d)\n", s->oid, po.x, po.y, po.z, pos.x, pos.y, pos.z, std::hypot(po.x - pos.x, po.y - pos.y), frame);
        if (s->follow)
        {
            // (follow: the budget's end is the step's end; within NEAR the pad stays neutral)
            if (frame - r.frameStep >= s->frames)
            {
                if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; std::printf("route: completed at frame %d\n", frame); }
                else r.fAdvance = true;
                return false;
            }
            if (std::hypot(po.x - pos.x, po.y - pos.y) <= s->near) return false;
        }
        sChase = *s;
        sChase.kind = STEPR::Move;
        sChase.x = po.x; sChase.y = po.y;
        if (s->behind > 0.0f && ploChased && static_cast<ALO*>(ploChased)->paloParent)
        {
            // (behind=R: the holder's back is the side the object hangs on; on the front side the target is beside the
            // holder at R (the side nearer the player) and 1.2 R behind it, until the player is behind)
            const glm::vec3 c = PosAloCenter(static_cast<ALO*>(ploChased)->paloParent);
            glm::vec2 b(po.x - c.x, po.y - c.y), d(pos.x - c.x, pos.y - c.y);
            const float lb = glm::length(b), ld = glm::length(d);
            if (lb > 1.0f && ld > 1.0f)
            {
                b /= lb;
                if (glm::dot(d / ld, b) < 0.6f)
                {
                    glm::vec2 side(-b.y, b.x);
                    if (glm::dot(side, d) < 0.0f) side = -side;
                    sChase.x = c.x + side.x * s->behind + b.x * s->behind * 1.2f;
                    sChase.y = c.y + side.y * s->behind + b.y * s->behind * 1.2f;
                    sChase.near = -1.0f;  // (not done on the way)
                }
            }
        }
        s = &sChase;
    }
    if (s->kind == STEPR::Move || s->kind == STEPR::Aim)
    {
        if (s->kind == STEPR::Move && (pos.z < s->zmin || pos.z > s->zmax))
        {
            std::printf("route: failed at frame %d: step %d's height %.0f outside %.0f..%.0f\n", frame, r.istep + 1, pos.z, s->zmin, s->zmax);
            r.fFailed = true;
            return false;
        }
        if (s->kind == STEPR::Move && frame - r.frameStep > s->frames)
        {
            std::printf("route: failed at frame %d: step %d's budget (at %.0f %.0f %.0f)\n", frame, r.istep + 1, pos.x, pos.y, pos.z);
            r.fFailed = true;
            return false;
        }
        const double dx = s->x - pos.x, dy = s->y - pos.y, d = std::hypot(dx, dy);
        if (s->kind == STEPR::Move && d <= s->near)
        {
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; std::printf("route: completed at frame %d\n", frame); }
            else r.fAdvance = true;
        }
        else if (d > 0)
        {
            const double ang = std::atan2(dy, dx) - std::atan2(camy, camx);
            a[0] = BStick(std::sin(ang), s->strength);
            a[1] = BStick(std::cos(ang), s->strength);
            if (s->run && d > 170) grf |= BTN_R1;
            if (s->jump > 0) { const int k = (rel - 1) % s->jump; if (k < 6 || (k >= 12 && k < 18)) grf |= BTN_Cross; }
        }
    }
    if (s->kind == STEPR::Wait && s->oidYaw >= 0 && g_psw && !r.fAdvance)
        for (const auto& up : g_psw->aploAll)
            if (up->oid == s->oidYaw && up->fInWorld && FCidDerivedFrom(up->Cid(), S3CID(0x30)))
            {
                ALO* palo = static_cast<ALO*>(up.get());
                const XF* pxf = PxfObj(palo);   // (the record's world matrix: the animated one)
                const glm::mat4 m = pxf ? pxf->matWorld : MatWorldOfAlo(palo);
                float a = glm::degrees(std::atan2(m[0][1], m[0][0]));
                a = std::fmod(std::fmod(a, 180.0f) + 180.0f, 180.0f);
                if (std::getenv("SLY2N_ROUTEYAW") && frame % 5 == 0) std::printf("route: yaw of %d %.1f (frame %d)\n", s->oidYaw, a, frame);
                if (a >= s->aYaw0 && a <= s->aYaw1)
                {
                    std::printf("route: step %d: %d's heading %.1f (frame %d t %.3f)\n", r.istep + 1, s->oidYaw, a, frame, g_clock.t);
                    r.fAdvance = true;
                }
                break;
            }
    if (s->kind == STEPR::Wait && s->fHold)
    {
        const double dx = s->xHold - pos.x, dy = s->yHold - pos.y, d = std::hypot(dx, dy);
        if (d > 30)
        {
            const double ang = std::atan2(dy, dx) - std::atan2(camy, camx);
            a[0] = BStick(std::sin(ang), std::min(1.0, d / 60.0));
            a[1] = BStick(std::cos(ang), std::min(1.0, d / 60.0));
        }
    }
    bool fAimOff = false;   // (point aimed=: the aim is off, the buttons wait)
    if (s->kind == STEPR::Point)
    {
        glm::vec3 tgt(s->x, s->y, s->z);
        if (s->cid >= 0)
        {
            glm::vec3 po;
            if (FNearestObj(s->cid, s->oid, s->x, s->y, &po)) tgt = po + glm::vec3(0.0f, 0.0f, s->dz);
        }
        else if (s->cid == -2 && g_psw)
            if (LO* plo = PloFindSwObjectByOid(g_psw, s->oid)) tgt = PosLoWorld(plo) + glm::vec3(0.0f, 0.0f, s->dz);
        // (look: the direction from the current player, as the look-at tutorials compare it with the camera's facing)
        LO* ploLook = s->fRight && g_psw ? PloPoCurrent(g_psw) : nullptr;
        const glm::vec3 d = tgt - (ploLook ? PosLoWorld(ploLook) : glm::vec3(g_cm.pos)), fw = g_cm.mat[0];
        double eYaw = std::atan2(d.y, d.x) - std::atan2(fw.y, fw.x);
        while (eYaw > M_PI) eYaw -= 2 * M_PI;
        while (eYaw < -M_PI) eYaw += 2 * M_PI;
        const double ePitch = std::atan2(d.z, std::hypot(d.x, d.y)) - std::atan2(fw.z, std::hypot(fw.x, fw.y));
        const auto dir = [](double e) { return std::fabs(e) < 0.003 ? 0.0 : std::copysign(std::clamp(std::fabs(e) * 60.0, 0.5, 1.0), e); };
        if (s->fRight)
        {
            // (look: the follow camera turned by the right stick; its pan rate falls with the stick squared, so the stick
            // eases off near the target)
            static const double s_sign = std::getenv("SLY3_LOOKSIGN") ? std::atof(std::getenv("SLY3_LOOKSIGN")) : -1.0;   // (the follow camera pans against the stick)
            const auto dirR = [](double e) { return std::fabs(e) < 0.01 ? 0.0 : std::copysign(std::clamp(std::sqrt(std::fabs(e)) * 0.9, 0.5, 1.0), e); };   // (0.5: clear of the pad's dead zone)
            a[2] = BStick(s_sign * dirR(eYaw), 1.0);
            static const double s_signY = std::getenv("SLY3_LOOKSIGNY") ? std::atof(std::getenv("SLY3_LOOKSIGNY")) : 1.0;
            a[3] = BStick(-s_signY * dirR(ePitch), 1.0);
        }
        else
        {
            a[0] = BStick(dir(eYaw), 1.0);
            a[1] = BStick(-dir(ePitch), 1.0);
        }
        if (s->aimed >= 0.0f && (std::fabs(eYaw) > s->aimed || std::fabs(ePitch) > s->aimed)) fAimOff = true;
        static const bool s_fPrint = std::getenv("SLY2N_POINTPRINT") != nullptr;
        if (s_fPrint) std::printf("point frame %d yaw %.5f pitch %.5f stick %u %u right %u %u target %.0f %.0f %.0f\n", frame, eYaw, ePitch, a[0], a[1], a[2], a[3], tgt.x, tgt.y, tgt.z);
    }
    const int relp = s->kind == STEPR::Point && s->every > 0 ? (rel - 1) % s->every + 1
                   : s->kind == STEPR::Tail || (s->kind == STEPR::Move && !s->apress.empty() && &r.astep[r.istep] != s) ? (rel - 1) % 60 + 1 : rel;
    if (s->kind == STEPR::Tail && s->fPocket && g_fRoutePocketPress) grf |= BTN_Circle;
    for (const PRESSR& p : s->apress)
        if (relp >= p.at && relp - p.at < p.c)
        {
            if (p.axis < 0) { if (!fAimOff) grf |= p.grf; }
            else a[p.axis] = p.b;
        }
    if (s->kind == STEPR::Photo && g_psw)
    {
        static bool s_fPress = false;
        static int s_relPress = 0;   // (the step's frame of the check: none while a password check holds the player)
        if (rel == 1) s_relPress = 0;
        if (!s_relPress && PloPoCurrent(g_psw) && !PloPasswordGuard3(PloPoCurrent(g_psw)))
        {
            s_relPress = rel;
            ACTOR* pa = PloPoCurrent(g_psw) ? PactorFromLo(PloPoCurrent(g_psw)) : nullptr;
            const int n = pa ? NPhotoPromptState(pa->binoc.win.pprompt) : -1;
            s_fPress = (n != -1) != (s->cid != 0);
            std::printf("route: step %d photo %s: view state %d%s (frame %d)\n", r.istep + 1, s->cid ? "on" : "off", n, s_fPress ? ", R3" : "", frame);
        }
        if (s_fPress && s_relPress && rel - s_relPress < 6) grf |= BTN_R3;
        if (s_relPress && rel - s_relPress >= 30)
        {
            // (the view not as wanted yet (R3 is not taken during a dialog or a scene): pressed again, until the budget)
            ACTOR* pa = PloPoCurrent(g_psw) ? PactorFromLo(PloPoCurrent(g_psw)) : nullptr;
            const int n = pa ? NPhotoPromptState(pa->binoc.win.pprompt) : -1;
            // (not while the binocucom is opening (+0xF04 0): a press seen once it is up closes it, orig3 0x1ACCA8)
            if (s_fPress && pa && pa->nF04 == 0 && rel < s->frames - 40) {}
            else if (s_fPress && (n != -1) != (s->cid != 0) && rel < s->frames - 40) s_relPress = rel;
            else r.fAdvance = true;
        }
    }
    static bool s_fAnswer = false;   // (from the first password step on, every later check is answered too)
    if (s->kind == STEPR::Password && g_psw)
    {
        static int s_c0 = 0;
        if (rel == 1) { s_c0 = CPasswordsPassed3(); s_fAnswer = true; }
        else if (CPasswordsPassed3() > s_c0 && PloPoCurrent(g_psw) && !PloPasswordGuard3(PloPoCurrent(g_psw)))
        {
            std::printf("route: step %d's password check passed (frame %d)\n", r.istep + 1, frame);
            r.fAdvance = true;
        }
    }
    if (s_fAnswer && g_psw)
        if (LO* plo = PloPoCurrent(g_psw))
        {
            // (a press of 2 frames, then 6 released: the check takes each new press within 0.15 s)
            static int s_frameLast = -100;
            const int btn = NPasswordButtonWanted3(plo);
            if (btn && frame - s_frameLast >= 8) s_frameLast = frame;
            if (btn && frame - s_frameLast < 2) grf |= uint16_t(btn << 8);   // (0x10 triangle .. 0x80 square: BTN_ >> 8)
        }
    if (s->fFire && g_psw)
    {
        float xT = s->xFire, yT = s->yFire, dBest = s->rFire;
        bool fT = s->fFirePoint;
        size_t iBest = SIZE_MAX;
        if (!fT)
            for (const auto& up : g_psw->aploAll)
            {
                LO* plo = up.get();
                if (!plo->fInWorld || !FCidDerivedFrom(plo->Cid(), S3CID(0x30))) continue;
                const auto it = std::find(s->aoidFire.begin(), s->aoidFire.end(), plo->oid);
                if (it == s->aoidFire.end()) continue;
                glm::vec3 p = PosAloCenter(static_cast<ALO*>(plo));
                // (a hack swarm: its live particle nearest to the player; none left: skipped)
                if (plo->Cid() == S3CID(0x4C) && !FHackSwarmNearestParticle(plo, pos, &p)) continue;
                const float d = std::hypot(p.x - pos.x, p.y - pos.y);
                const size_t i = !s->fFirePrio || d < s->rFireNear ? 0 : size_t(it - s->aoidFire.begin()) + 1;
                if (d >= s->rFire) continue;
                if (i < iBest || (i == iBest && d < dBest)) { iBest = i; dBest = d; xT = p.x; yT = p.y; fT = true; }
            }
        if (!fT && s->fUntilClear && frame - r.frameStep >= 30 && !r.fAdvance)
        {
            std::printf("route: step %d: nothing left to fire at (frame %d t %.3f, player at %.0f %.0f)\n", r.istep + 1, frame, g_clock.t, pos.x, pos.y);
            r.fAdvance = true;
        }
        if (fT)
        {
            const double ang = std::atan2(yT - pos.y, xT - pos.x) - std::atan2(camy, camx);
            a[2] = BStick(std::sin(ang), 1.0);
            a[3] = BStick(std::cos(ang), 1.0);
        }
    }
    grf |= r.grfHeld;
    *pgrf = grf;
    for (int i = 0; i < 4; ++i) abAxis[i] = a[i];
    if (r.pfTrace)
        std::fprintf(r.pfTrace, "%d,%.4f,%d,%u,%u,%u,%.2f,%.2f,%.2f,%.5f\n", frame, g_clock.t, r.istep + 1, grf, a[0], a[1], pos.x, pos.y, pos.z,
                     std::atan2(camy, camx));
    return true;
}
